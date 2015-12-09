//
//  Encoder.cc
//  Fleece
//
//  Created by Jens Alfke on 1/26/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "Encoder.hh"
#include "Endian.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>
#include <stdlib.h>


namespace fleece {

    typedef uint8_t byte;

    using namespace internal;

    Encoder::Encoder(Writer &out)
    :_out(out),
     _stackDepth(0),
     _strings(100),
     _uniqueStrings(true),
     _sortKeys(true),
     _writingKey(false),
     _blockedOnKey(false)
    {
        push(kSpecialTag, 1);                   // Top-level 'array' is just a single item
#ifndef NDEBUG
        _numNarrow = _numWide = _narrowCount = _wideCount = _numSavedStrings = 0;
#endif
    }

    Encoder::~Encoder() {
        end();
    }

    void Encoder::end() {
        if (!_items)
            return;
        if (_stackDepth > 1)
            throw "unclosed array/dict";
        if (_items->size() > 1)
            throw "top level must have only one value";

        if (_items->size() > 0) {
            fixPointers(_items);
            value &root = (*_items)[0];
            if (_items->wide) {
                _out.write(&root, kWide);
                // Top level value is 4 bytes, so append a 2-byte pointer to it, because the trailer
                // needs to be a 2-byte value:
                value ptr(4, kNarrow);
                _out.write(&ptr, kNarrow);
            } else {
                _out.write(&root, kNarrow);
            }
            _items->clear();
        }
        _items = NULL;
        _stackDepth = 0;
    }

    // Returns position in the stream of the next write. Pads stream to even pos if necessary.
    size_t Encoder::nextWritePos() {
        size_t pos = _out.length();
        if (pos & 1) {
            byte zero = 0;
            _out.write(&zero, 1);
            pos++;
        }
        return pos;
    }

    void Encoder::reset() {
        if (_items) {
            _items->clear();
            _items = NULL;
        }
        _out = Writer();
        _stackDepth = 0;
        push(kSpecialTag, 1);
        _strings.clear();
        _writingKey = _blockedOnKey = false;
    }


#pragma mark - WRITING:

    void Encoder::addItem(value v) {
        if (_blockedOnKey)
            throw "need a key before this value";
        if (_writingKey) {
            _writingKey = false;
        } else {
            if (_items->tag == kDictTag)
                _blockedOnKey = _writingKey = true;
        }

        _items->push_back(v);
    }

    void Encoder::writeValue(tags tag, byte buf[], size_t size, bool canInline) {
        buf[0] |= tag << 4;
        if (canInline && size <= 4) {
            if (size < 4)
                memset(&buf[size], 0, 4-size); // zero unused bytes
            addItem(*(value*)buf);
            if (size > 2)
                _items->wide = true;
        } else {
            writePointer(nextWritePos());
            _out.write(buf, size);
        }
    }


#pragma mark - SCALARS:

    void Encoder::writeNull()              {addItem(value(kSpecialTag, kSpecialValueNull));}
    void Encoder::writeBool(bool b)        {addItem(value(kSpecialTag, b ? kSpecialValueTrue
                                                                       : kSpecialValueFalse));}
    void Encoder::writeInt(uint64_t i, bool isSmall, bool isUnsigned) {
        if (isSmall) {
            addItem(value(kShortIntTag, (i >> 8) & 0x0F, i & 0xFF));
        } else {
            byte buf[10];
            size_t size = PutIntOfLength(&buf[1], i, isUnsigned);
            buf[0] = (byte)size - 1;
            if (isUnsigned)
                buf[0] |= 0x08;
            ++size;
            if (size & 1)
                buf[size++] = 0;  // pad to even size
            writeValue(kIntTag, buf, size);
        }
    }

    void Encoder::writeInt(int64_t i)   {writeInt(i, (i < 2048 && i >= -2048), false);}
    void Encoder::writeUInt(uint64_t i) {writeInt(i, (i < 2048),               true);}

    void Encoder::writeDouble(double n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int64_t)n) {
            return writeInt((int64_t)n);
        } else {
            littleEndianDouble swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = 0x08; // 'double' size flag
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(kFloatTag, buf, sizeof(buf));
        }
    }

    void Encoder::writeFloat(float n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int32_t)n) {
            writeInt((int32_t)n);
        } else {
            littleEndianFloat swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = 0x00; // 'float' size flag
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(kFloatTag, buf, sizeof(buf));
        }
    }


#pragma mark - STRINGS / DATA:

    // used for strings and binary data. Returns the location where s got written to, which
    // can be used until the enoding is over. (Unless it's inline, in which case s.buf is NULL.)
    slice Encoder::writeData(tags tag, slice s) {
        uint8_t buf[4 + kMaxVarintLen64];
        buf[0] = (uint8_t)std::min(s.size, (size_t)0xF);
        const void *dst;
        if (s.size < kNarrow) {
            // Tiny data fits inline:
            memcpy(&buf[1], s.buf, s.size);
            writeValue(tag, buf, 1 + s.size);
            dst = NULL;
        } else {
            // Large data doesn't:
            size_t bufLen = 1;
            if (s.size >= 0x0F)
                bufLen += PutUVarInt(&buf[1], s.size);
            if (s.size == 0)
                buf[bufLen++] = 0;
            writeValue(tag, buf, bufLen, false);       // write header/count
            dst = _out.write(s.buf, s.size);
        }
        return slice(dst, s.size);
    }

    // Returns the location where s got written to, if possible, just like writeData above.
    slice Encoder::_writeString(slice s, bool asKey) {
        // Check whether this string's already been written:
        if (__builtin_expect(_uniqueStrings && s.size >= kNarrow && s.size <= kMaxSharedStringSize,
                             true)) {
            auto entry = _strings.find(s);
            if (entry->first.buf != NULL) {
//                fprintf(stderr, "Found `%.*s` --> %u\n", (int)s.size, s.buf, entry->second);
                writePointer(entry->second.offset);
#ifndef NDEBUG
                _numSavedStrings++;
#endif
                if (asKey)
                    entry->second.usedAsKey = true;
                return entry->first;
            } else {
                auto offset = (uint32_t)nextWritePos();
                s = writeData(kStringTag, s);
                if (s.buf) {
#if 0
                    if (_strings.count() == 0)
                        fprintf(stderr, "---- new encoder ----\n");
                    fprintf(stderr, "Caching `%.*s` --> %u\n", (int)s.size, s.buf, offset);
#endif
                    StringTable::info i = {offset, asKey};
                    _strings.addAt(entry, s, i);
                }
                return s;
            }
        } else {
            return writeData(kStringTag, s);
        }
    }

    void Encoder::writeString(std::string s) {
        _writeString(slice(s), false);
    }

    void Encoder::writeData(slice s) {
        writeData(kBinaryTag, s);
    }


#pragma mark - POINTERS:

    // Pointers are added here as absolute positions in the stream (and fixed up before writing)
    void Encoder::writePointer(size_t p)   {addItem(value(p, kWide));}

    // Check whether any pointers in _items can't fit in a narrow value:
    void Encoder::checkPointerWidths(valueArray *items) {
        if (!items->wide) {
            size_t base = nextWritePos();
            for (auto v = items->begin(); v != items->end(); ++v) {
                if (v->isPointer()) {
                    size_t pos = v->pointerValue<true>();
                    if (base - pos >= 0x10000) {
                        items->wide = true;
                        break;
                    }
                }
                base += kNarrow;
            }
        }
    }

    // Convert absolute offsets to relative in _items:
    void Encoder::fixPointers(valueArray *items) {
        size_t base = nextWritePos();
        int width = items->wide ? kWide : kNarrow;
        for (auto v = items->begin(); v != items->end(); ++v) {
            if (v->isPointer()) {
                size_t pos = v->pointerValue<true>();
                assert(pos < base);
                pos = base - pos;
                *v = value(pos, width);
            }
            base += width;
        }
    }

#pragma mark - ARRAYS / DICTIONARIES:

    void Encoder::writeKey(std::string s)   {writeKey(slice(s));}

    void Encoder::writeKey(slice s) {
        if (!_blockedOnKey) {
            if (_items->tag == kDictTag)
                throw "need a value after a key";
            else
                throw "not writing a dictionary";
        }
        _blockedOnKey = false;
        s = _writeString(s, true);
        if (_sortKeys)
            _items->keys.push_back(s);
    }

    void Encoder::push(tags tag, size_t reserve) {
        if (_stackDepth >= _stack.size())
            _stack.emplace_back();
        _items = &_stack[_stackDepth++];
        _items->reset(tag);
        if (reserve > 0)
            _items->reserve(reserve);
    }

    void Encoder::beginArray(size_t reserve) {
        push(kArrayTag, reserve);
    }

    void Encoder::beginDictionary(size_t reserve) {
        push(kDictTag, 2*reserve);
        _writingKey = _blockedOnKey = true;
    }

    void Encoder::endArray() {
        endCollection(internal::kArrayTag);
    }

    void Encoder::endDictionary() {
        if (!_writingKey)
            throw "need a value";
        endCollection(internal::kDictTag);
    }

    void Encoder::endCollection(tags tag) {
        if (_items->tag != tag)
            throw "ending wrong type of collection";

        // Pop _items off the stack:
        valueArray *items = _items;
        --_stackDepth;
        _items = &_stack[_stackDepth - 1];
        _writingKey = _blockedOnKey = false;

        if (_sortKeys && tag == kDictTag)
            sortDict(*items);

        checkPointerWidths(items);

        auto count = (uint32_t)items->size();    // includes keys if this is a dict!
        if (items->tag == kDictTag)
            count /= 2;

        // Write the array header to the outer value:
        uint8_t buf[2 + kMaxVarintLen32];
        uint32_t inlineCount = std::min(count, (uint32_t)0x07FF);
        buf[0] = (uint8_t)(inlineCount >> 8);
        buf[1] = (uint8_t)(inlineCount & 0xFF);
        size_t bufLen = 2;
        if (count >= 0x0FFF) {
            bufLen += PutUVarInt(&buf[2], count);
            if (bufLen & 1)
                buf[bufLen++] = 0;
        }
        if (items->wide)
            buf[0] |= 0x08;     // "wide" flag
        writeValue(items->tag, buf, bufLen, (count==0));          // can inline only if empty

        fixPointers(items);

        // Write the values:
        if (count > 0) {
            auto nValues = items->size();
            if (items->wide) {
                _out.write(&(*items)[0], kWide*nValues);
            } else {
                uint16_t narrow[nValues];
                size_t i = 0;
                for (auto v = items->begin(); v != items->end(); ++v, ++i) {
                    ::memcpy(&narrow[i], &*v, kNarrow);
                }
                _out.write(narrow, kNarrow*nValues);
            }
        }

#ifndef NDEBUG
        if (items->wide) {
            _numWide++;
            _wideCount += count;
        } else {
            _numNarrow++;
            _narrowCount += count;
        }
#endif

        items->clear();
    }

    static int compareKeysByIndex(const void *a, const void *b) {
        auto &sa = **(const slice**)a;
        auto &sb = **(const slice**)b;
        assert(sa.buf && sb.buf);
        return sa.compare(sb);
    }

    void Encoder::sortDict(valueArray &items) {
        auto &keys = items.keys;
        size_t n = keys.size();
        if (n < 2)
            return;

        // Fill in the pointers of any keys that refer to inline strings:
        for (unsigned i = 0; i < n; i++)
            if (keys[i].buf == NULL)
                keys[i].buf = offsetby(&items[2*i], 1);

        // Construct an array that describes the permutation of item indices:
        const slice* indices[n];
        const slice* base = &keys[0];
        for (unsigned i = 0; i < n; i++)
            indices[i] = base + i;
        ::qsort(indices, n, sizeof(indices[0]), &compareKeysByIndex);
        // indices[i] is now a pointer to the value that should go at index i

        // Now rewrite items according to the permutation in indices:
        value *old = (value*) alloca(2*n * sizeof(value));
        memcpy(old, &items[0], 2*n * sizeof(value));
        for (size_t i = 0; i < n; i++) {
            auto j = indices[i] - base;
            if ((ssize_t)i != j) {
                items[2*i]   = old[2*j];
                items[2*i+1] = old[2*j+1];
            }
        }
    }

    void Encoder::writeKeyTable() {
        StringTable keys;
        for (auto iter = _strings.begin(); iter != _strings.end(); ++iter) {
            if (iter->buf && iter.value().usedAsKey)
                keys.add(iter, iter.value());
        }

        beginArray();
        for (auto iter = keys.begin(); iter != keys.end(); ++iter) {
            if (iter->buf) {
                writeString(iter);
            } else {
                writeNull();
            }
        }
        endArray();
    }
}
