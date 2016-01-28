//
//  BugsnagKSJSONCodec.c
//
//  Created by Karl Stenerud on 2012-01-07.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include "BugsnagKSJSONCodec.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ============================================================================
#pragma mark - Configuration -
// ============================================================================

/** Set to 1 if you're also compiling BugsnagKSLogger and want to use it here */
#ifndef BugsnagKSJSONCODEC_UseBugsnagKSLogger
    #define BugsnagKSJSONCODEC_UseBugsnagKSLogger 1
#endif

#if BugsnagKSJSONCODEC_UseBugsnagKSLogger
    #include "BugsnagKSLogger.h"
#else
    #define BugsnagKSLOG_DEBUG(FMT, ...)
#endif

/** The work buffer size to use when escaping string values.
 * There's little reason to change this since nothing ever gets truncated.
 */
#ifndef BugsnagKSJSONCODEC_WorkBufferSize
    #define BugsnagKSJSONCODEC_WorkBufferSize 512
#endif


// ============================================================================
#pragma mark - Helpers -
// ============================================================================

// Compiler hints for "if" statements
#define likely_if(x) if(__builtin_expect(x,1))
#define unlikely_if(x) if(__builtin_expect(x,0))

/** Used for writing hex string values. */
static char g_hexNybbles[] =
{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

const char* bugsnag_ksjson_stringForError(const int error)
{
    switch (error)
    {
        case BugsnagKSJSON_ERROR_INVALID_CHARACTER:
            return "Invalid character";
        case BugsnagKSJSON_ERROR_CANNOT_ADD_DATA:
            return "Cannot add data";
        case BugsnagKSJSON_ERROR_INCOMPLETE:
            return "Incomplete data";
        case BugsnagKSJSON_ERROR_INVALID_DATA:
            return "Invalid data";
        default:
            return "(unknown error)";
    }
}


// ============================================================================
#pragma mark - Encode -
// ============================================================================

// Avoiding static functions due to linker issues.

/** Add JSON encoded data to an external handler.
 * The external handler will decide how to handle the data (store/transmit/etc).
 *
 * @param context The encoding context.
 *
 * @param data The encoded data.
 *
 * @param length The length of the data.
 *
 * @return true if the data was handled successfully.
 */
#define addJSONData(CONTEXT,DATA,LENGTH) \
    (CONTEXT)->addJSONData(DATA, LENGTH, (CONTEXT)->userData)

/** Escape a string portion for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bugsnag_ksjsoncodec_i_appendEscapedString(BugsnagKSJSONEncodeContext* const context,
                                      const char* restrict const string,
                                      size_t length)
{
    char workBuffer[BugsnagKSJSONCODEC_WorkBufferSize];
    const char* const srcEnd = string + length;

    const char* restrict src = string;
    char* restrict dst = workBuffer;

    // Simple case (no escape or special characters)
    for(; src < srcEnd &&
        *src != '\\' &&
        *src != '\"' &&
        (unsigned char)*src >= ' '; src++)
    {
        *dst++ = *src;
    }

    // Deal with complicated case (if any)
    for(; src < srcEnd; src++)
    {
        switch(*src)
        {
            case '\\':
            case '\"':
                *dst++ = '\\';
                *dst++ = *src;
                break;
            case '\b':
                *dst++ = '\\';
                *dst++ = 'b';
                break;
            case '\f':
                *dst++ = '\\';
                *dst++ = 'f';
                break;
            case '\n':
                *dst++ = '\\';
                *dst++ = 'n';
                break;
            case '\r':
                *dst++ = '\\';
                *dst++ = 'r';
                break;
            case '\t':
                *dst++ = '\\';
                *dst++ = 't';
                break;
            default:
                unlikely_if((unsigned char)*src < ' ')
            {
                BugsnagKSLOG_DEBUG("Invalid character 0x%02x in string: %s",
                            *src, string);
                return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
            }
                *dst++ = *src;
        }
    }
    size_t encLength = (size_t)(dst - workBuffer);
    dst -= encLength;
    return addJSONData(context, dst, encLength);
}

/** Escape a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bugsnag_ksjsoncodec_i_addEscapedString(BugsnagKSJSONEncodeContext* const context,
                                   const char* restrict const string,
                                   size_t length)
{
    int result = BugsnagKSJSON_OK;

    // Keep adding portions until the whole string has been processed.
    size_t offset = 0;
    while(offset < length)
    {
        size_t toAdd = length - offset;
        unlikely_if(toAdd > BugsnagKSJSONCODEC_WorkBufferSize / 2)
        {
            toAdd = BugsnagKSJSONCODEC_WorkBufferSize / 2;
        }
        result = bugsnag_ksjsoncodec_i_appendEscapedString(context,
                                                   string + offset,
                                                   toAdd);
        unlikely_if(result != BugsnagKSJSON_OK)
        {
            break;
        }
        offset += toAdd;
    }
    return result;
}

/** Escape and quote a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bugsnag_ksjsoncodec_i_addQuotedEscapedString(BugsnagKSJSONEncodeContext* const context,
                                         const char* restrict const string,
                                         size_t length)
{
    int result;
    unlikely_if((result = addJSONData(context, "\"", 1)) != BugsnagKSJSON_OK)
    {
        return result;
    }
    unlikely_if((result = bugsnag_ksjsoncodec_i_addEscapedString(context,
                                                         string,
                                                         length)) != BugsnagKSJSON_OK)
    {
        return result;
    }
    return addJSONData(context, "\"", 1);
}

/** Add any necessary JSON preamble text, including commas and names.
 *
 * @param context The JSON context.
 *
 * @param The name of the next element (only needed if parent is a dictionary).
 */
int bugsnag_ksjsoncodec_i_addPreamble(BugsnagKSJSONEncodeContext* const context,
                              const char* const name)
{
    int result = BugsnagKSJSON_OK;

    // Decide if a comma is warranted.
    unlikely_if(context->containerFirstEntry)
    {
        context->containerFirstEntry = false;
    }
    else
    {
        unlikely_if((result = addJSONData(context, ",", 1)) != BugsnagKSJSON_OK)
        {
            return result;
        }
    }

    // Pretty printing
    unlikely_if(context->prettyPrint && context->containerLevel > 0)
    {
        unlikely_if((result = addJSONData(context, "\n", 1)) != BugsnagKSJSON_OK)
        {
            return result;
        }
        for(int i = 0; i < context->containerLevel; i++)
        {
            unlikely_if((result = addJSONData(context, "    ", 4)) != BugsnagKSJSON_OK)
            {
                return result;
            }
        }
    }

    // Add a name field if we're in an object.
    if(context->isObject[context->containerLevel])
    {
        unlikely_if(name == NULL)
        {
            BugsnagKSLOG_DEBUG("Name was null inside an object");
            return BugsnagKSJSON_ERROR_INVALID_DATA;
        }
        unlikely_if((result = bugsnag_ksjsoncodec_i_addQuotedEscapedString(context,
                                                                   name,
                                                                   strlen(name))) != BugsnagKSJSON_OK)
        {
            return result;
        }
        unlikely_if(context->prettyPrint)
        {
            unlikely_if((result = addJSONData(context, ": ", 2)) != BugsnagKSJSON_OK)
            {
                return result;
            }
        }
        else
        {
            unlikely_if((result = addJSONData(context, ":", 1)) != BugsnagKSJSON_OK)
            {
                return result;
            }
        }
    }
    return result;
}

int bugsnag_ksjson_addBooleanElement(BugsnagKSJSONEncodeContext* const context,
                             const char* const name,
                             const bool value)
{
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    if(value)
    {
        return addJSONData(context, "true", 4);
    }
    else
    {
        return addJSONData(context, "false", 5);
    }
}

int bugsnag_ksjson_addFloatingPointElement(BugsnagKSJSONEncodeContext* const context,
                                   const char* const name,
                                   double value)
{
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    char buff[30];
    sprintf(buff, "%lg", value);
    return addJSONData(context, buff, strlen(buff));
}

int bugsnag_ksjson_addIntegerElement(BugsnagKSJSONEncodeContext* const context,
                             const char* const name,
                             long long value)
{
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    char buff[30];
    sprintf(buff, "%lld", value);
    return addJSONData(context, buff, strlen(buff));
}

int bugsnag_ksjson_addJSONElement(BugsnagKSJSONEncodeContext* const context,
                          const char* restrict const name,
                          const char* restrict const element,
                          size_t length)
{
    unlikely_if(element == NULL)
    {
        return bugsnag_ksjson_addNullElement(context, name);
    }
    size_t idx = 0;
    while(idx < length &&
          (element[idx] == ' ' ||
           element[idx] == '\r' ||
           element[idx] == '\n' ||
           element[idx] == '\t' ||
           element[idx] == '\f'))
    {
        idx++;
    }
    unlikely_if(idx >= length)
    {
        BugsnagKSLOG_DEBUG("JSON element contained no JSON data: %s", element);
        return BugsnagKSJSON_ERROR_INVALID_DATA;
    }
    switch(element[idx])
    {
        case '[': case '{': case '\"': case 'f':
        case 't': case 'n': case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            break;
        default:
            BugsnagKSLOG_DEBUG("Invalid character '%c' in: ",
                        element[idx], element);
            return BugsnagKSJSON_ERROR_INVALID_DATA;
    }

    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    return addJSONData(context, element, length);
}

int bugsnag_ksjson_addNullElement(BugsnagKSJSONEncodeContext* const context,
                          const char* const name)
{
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    return addJSONData(context, "null", 4);
}

int bugsnag_ksjson_addStringElement(BugsnagKSJSONEncodeContext* const context,
                            const char* const name,
                            const char* const value,
                            size_t length)
{
    unlikely_if(value == NULL)
    {
        return bugsnag_ksjson_addNullElement(context, name);
    }
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    return bugsnag_ksjsoncodec_i_addQuotedEscapedString(context, value, length);
}

int bugsnag_ksjson_beginStringElement(BugsnagKSJSONEncodeContext* const context,
                              const char* const name)
{
    int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
    unlikely_if(result != BugsnagKSJSON_OK)
    {
        return result;
    }
    return addJSONData(context, "\"", 1);
}

int bugsnag_ksjson_appendStringElement(BugsnagKSJSONEncodeContext* const context,
                               const char* const value,
                               size_t length)
{
    return bugsnag_ksjsoncodec_i_addEscapedString(context, value, length);
}

int bugsnag_ksjson_endStringElement(BugsnagKSJSONEncodeContext* const context)
{
    return addJSONData(context, "\"", 1);
}

int bugsnag_ksjson_addDataElement(BugsnagKSJSONEncodeContext* const context,
                          const char* name,
                          const char* value,
                          size_t length)
{
    int result = BugsnagKSJSON_OK;
    result = bugsnag_ksjson_beginDataElement(context, name);
    if(result == BugsnagKSJSON_OK)
    {
        result = bugsnag_ksjson_appendDataElement(context, value, length);
    }
    if(result == BugsnagKSJSON_OK)
    {
        result = bugsnag_ksjson_endDataElement(context);
    }
    return result;
}

int bugsnag_ksjson_beginDataElement(BugsnagKSJSONEncodeContext* const context,
                            const char* const name)
{
    return bugsnag_ksjson_beginStringElement(context, name);
}

int bugsnag_ksjson_appendDataElement(BugsnagKSJSONEncodeContext* const context,
                             const char* const value,
                             size_t length)
{
    unsigned char* currentByte = (unsigned char*)value;
    unsigned char* end = currentByte + length;
    char chars[2];
    int result = BugsnagKSJSON_OK;
    while(currentByte < end)
    {
        chars[0] = g_hexNybbles[(*currentByte>>4)&15];
        chars[1] = g_hexNybbles[*currentByte&15];
        result = addJSONData(context, chars, sizeof(chars));
        if(result != BugsnagKSJSON_OK)
        {
            break;
        }
        currentByte++;
    }
    return result;
}

int bugsnag_ksjson_endDataElement(BugsnagKSJSONEncodeContext* const context)
{
    return bugsnag_ksjson_endStringElement(context);
}

int bugsnag_ksjson_beginArray(BugsnagKSJSONEncodeContext* const context,
                      const char* const name)
{
    likely_if(context->containerLevel >= 0)
    {
        int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
        unlikely_if(result != BugsnagKSJSON_OK)
        {
            return result;
        }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = false;
    context->containerFirstEntry = true;

    return addJSONData(context, "[", 1);
}

int bugsnag_ksjson_beginObject(BugsnagKSJSONEncodeContext* const context,
                       const char* const name)
{
    likely_if(context->containerLevel >= 0)
    {
        int result = bugsnag_ksjsoncodec_i_addPreamble(context, name);
        unlikely_if(result != BugsnagKSJSON_OK)
        {
            return result;
        }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = true;
    context->containerFirstEntry = true;

    return addJSONData(context, "{", 1);
}

int bugsnag_ksjson_endContainer(BugsnagKSJSONEncodeContext* const context)
{
    unlikely_if(context->containerLevel <= 0)
    {
        return BugsnagKSJSON_OK;
    }

    bool isObject = context->isObject[context->containerLevel];
    context->containerLevel--;

    // Pretty printing
    unlikely_if(context->prettyPrint && !context->containerFirstEntry)
    {
        int result;
        unlikely_if((result = addJSONData(context, "\n", 1)) != BugsnagKSJSON_OK)
        {
            return result;
        }
        for(int i = 0; i < context->containerLevel; i++)
        {
            unlikely_if((result = addJSONData(context, "    ", 4)) != BugsnagKSJSON_OK)
            {
                return result;
            }
        }
    }
    context->containerFirstEntry = false;
    return addJSONData(context, isObject ? "}" : "]", 1);
}

void bugsnag_ksjson_beginEncode(BugsnagKSJSONEncodeContext* const context,
                        bool prettyPrint,
                        BugsnagKSJSONAddDataFunc addJSONDataFunc,
                        void* const userData)
{
    memset(context, 0, sizeof(*context));
    context->addJSONData = addJSONDataFunc;
    context->userData = userData;
    context->prettyPrint = prettyPrint;
    context->containerFirstEntry = true;
}

int bugsnag_ksjson_endEncode(BugsnagKSJSONEncodeContext* const context)
{
    int result = BugsnagKSJSON_OK;
    while(context->containerLevel > 0)
    {
        unlikely_if((result = bugsnag_ksjson_endContainer(context)) != BugsnagKSJSON_OK)
        {
            return result;
        }
    }
    return result;
}


// ============================================================================
#pragma mark - Decode -
// ============================================================================

#define INV 0x11111

/** Lookup table for converting hex values to integers.
 * INV (0x11111) is used to mark invalid characters so that any attempted
 * invalid nybble conversion is always > 0xffff.
 */
static const unsigned int g_hexConversion[] =
{
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, INV, INV, INV, INV, INV, INV,
    INV, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
};


/** Encode a UTF-16 character to UTF-8. The dest pointer gets incremented
 * by however many bytes were needed for the conversion (1-4).
 *
 * @param character The UTF-16 character.
 *
 * @param dst Where to write the UTF-8 character.
 *
 * @return BugsnagKSJSON_OK if the encoding was successful.
 */
int bugsnag_ksjsoncodec_i_writeUTF8(unsigned int character, char** dst);

/** Decode a string value. The newly allocated string is stored in dstString
 * (only if parsing was successful) and it is the responsibility of the caller
 * to free() it. Do not call free() on *dstString if parsing fails.
 *
 * @param ptr Pointer-to-pointer to the input data (will be modified).
 *
 * @param end Marks the end of the input data.
 *
 * @param dstString Stores the newly allocated string pointer (if successful).
 *                  If parsing fails, nothing is written here.
 *
 * @return BugsnagKSJSON_OK if successful.
 */
int bugsnag_ksjsoncodec_i_decodeString(const char** ptr,
                               const char* const end,
                               char** dstString);

/** Decode a JSON element.
 *
 * @param ptr Pointer-to-pointer to the input data (will be modified).
 *
 * @param end Marks the end of the input data.
 *
 * @param name This element's name (or NULL if it has none).
 *
 * @param callbacks The callbacks to call while decoding.
 *
 * @param userData Data that was specified when calling bugsnag_ksjson_decode().
 *
 * @return BugsnagKSJSON_OK if successful.
 */
int bugsnag_ksjsoncodec_i_decodeElement(const char** ptr,
                                const char* const end,
                                const char* const name,
                                BugsnagKSJSONDecodeCallbacks* const callbacks,
                                void* const userData);


/** Skip past any whitespace.
 *
 * @param ptr pointer to the pointer to the characters. On exit, *ptr will
 *            point past any whitespace found.
 *
 * @param end Marks the end of the characters.
 */
static inline void skipWhitespace(const char** ptr,
                                  const char* const end)
{
    while(*ptr < end && isspace(**ptr))
    {
        (*ptr)++;
    }
}

/** Check if a character is valid for representing part of a floating point
 * number.
 *
 * @param ch The character to test.
 *
 * @return true if the character is valid for floating point.
 */
static inline bool isFPChar(char ch)
{
    switch(ch)
    {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case '.': case 'e': case 'E': case '+': case '-':
            return true;
        default:
            return false;
    }
}

int bugsnag_ksjsoncodec_i_writeUTF8(unsigned int character, char** dst)
{
    likely_if(character <= 0x7f)
    {
        **dst = (char) character;
        (*dst)++;
        return BugsnagKSJSON_OK;
    }
    if(character <= 0x7ff)
    {
        (*dst)[0] = (char)(0xc0 | (character >> 6));
        (*dst)[1] = (char)(0x80 | (character & 0x3f));
        *dst += 2;
        return BugsnagKSJSON_OK;
    }
    if(character <= 0xffff)
    {
        (*dst)[0] = (char)(0xe0 | (character >> 12));
        (*dst)[1] = (char)(0x80 | ((character >> 6) & 0x3f));
        (*dst)[2] = (char)(0x80 | (character & 0x3f));
        *dst += 3;
        return BugsnagKSJSON_OK;
    }
    // RFC3629 restricts UTF-8 to end at 0x10ffff.
    if(character <= 0x10ffff)
    {
        (*dst)[0] = (char)(0xf0 | (character >> 18));
        (*dst)[1] = (char)(0x80 | ((character >> 12) & 0x3f));
        (*dst)[2] = (char)(0x80 | ((character >> 6) & 0x3f));
        (*dst)[3] = (char)(0x80 | (character & 0x3f));
        *dst += 4;
        return BugsnagKSJSON_OK;
    }

    // If we get here, the character cannot be converted to valid UTF-8.
    BugsnagKSLOG_DEBUG("Invalid unicode: 0x%04x", character);
    return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
}

int bugsnag_ksjsoncodec_i_decodeString(const char** ptr,
                               const char* const end,
                               char** dstString)
{
    unlikely_if(**ptr != '\"')
    {
        BugsnagKSLOG_DEBUG("Expected '\"' but got '%c'", **ptr);
        return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
    }

    const char* src = *ptr + 1;
    bool fastCopy = true;

    for(; src < end && *src != '\"'; src++)
    {
        unlikely_if(*src == '\\')
        {
            fastCopy = false;
            src++;
        }
    }
    unlikely_if(src >= end)
    {
        BugsnagKSLOG_DEBUG("Premature end of data");
        return BugsnagKSJSON_ERROR_INCOMPLETE;
    }
    const char* const srcEnd = src;
    src = *ptr + 1;
    size_t length = (size_t)(srcEnd - src);

    int result = BugsnagKSJSON_OK;
    char* string = malloc(length + 1);

    // If no escape characters were encountered, we can fast copy.
    likely_if(fastCopy)
    {
        memcpy(string, src, length);
        string[length] = 0;
        *dstString = string;
        *ptr += length + 2;
        return BugsnagKSJSON_OK;
    }

    char* dst = string;

    for(; src < srcEnd; src++)
    {
        likely_if(*src != '\\')
        {
            *dst++ = *src;
        }
        else
        {
            src++;
            switch(*src)
            {
                case '"':
                    *dst++ = '\"';
                    continue;
                case '\\':
                    *dst++ = '\\';
                    continue;
                case 'n':
                    *dst++ = '\n';
                    continue;
                case 'r':
                    *dst++ = '\r';
                    continue;
                case '/':
                    *dst++ = '/';
                    continue;
                case 't':
                    *dst++ = '\t';
                    continue;
                case 'b':
                    *dst++ = '\b';
                    continue;
                case 'f':
                    *dst++ = '\f';
                    continue;
                case 'u':
                {
                    unlikely_if(src + 5 > srcEnd)
                    {
                        BugsnagKSLOG_DEBUG("Premature end of data");
                        result = BugsnagKSJSON_ERROR_INCOMPLETE;
                        goto failed;
                    }
                    unsigned int accum =
                    g_hexConversion[src[1]] << 12 |
                    g_hexConversion[src[2]] << 8 |
                    g_hexConversion[src[3]] << 4 |
                    g_hexConversion[src[4]];
                    unlikely_if(accum > 0xffff)
                    {
                        BugsnagKSLOG_DEBUG("Invalid unicode sequence: %c%c%c%c",
                                    src[1], src[2], src[3], src[4]);
                        result = BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                        goto failed;
                    }

                    // UTF-16 Trail surrogate on its own.
                    unlikely_if(accum >= 0xdc00 && accum <= 0xdfff)
                    {
                        BugsnagKSLOG_DEBUG("Unexpected trail surrogate: 0x%04x",
                                    accum);
                        result = BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                        goto failed;
                    }

                    // UTF-16 Lead surrogate.
                    unlikely_if(accum >= 0xd800 && accum <= 0xdbff)
                    {
                        // Fetch trail surrogate.
                        unlikely_if(src + 11 > srcEnd)
                        {
                            BugsnagKSLOG_DEBUG("Premature end of data");
                            result = BugsnagKSJSON_ERROR_INCOMPLETE;
                            goto failed;
                        }
                        unlikely_if(src[5] != '\\' ||
                                    src[6] != 'u')
                        {
                            BugsnagKSLOG_DEBUG("Expected \"\\u\" but got: \"%c%c\"",
                                        src[5], src[6]);
                            result = BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                            goto failed;
                        }
                        src += 6;
                        unsigned int accum2 =
                        g_hexConversion[src[1]] << 12 |
                        g_hexConversion[src[2]] << 8 |
                        g_hexConversion[src[3]] << 4 |
                        g_hexConversion[src[4]];
                        unlikely_if(accum2 < 0xdc00 || accum2 > 0xdfff)
                        {
                            BugsnagKSLOG_DEBUG("Invalid trail surrogate: 0x%04x",
                                        accum2);
                            result = BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                            goto failed;
                        }
                        // And combine 20 bit result.
                        accum = ((accum - 0xd800) << 10) | (accum2 - 0xdc00);
                    }

                    result = bugsnag_ksjsoncodec_i_writeUTF8(accum, &dst);
                    unlikely_if(result != BugsnagKSJSON_OK)
                    {
                        goto failed;
                    }
                    src += 4;
                    continue;
                }
                default:
                    BugsnagKSLOG_DEBUG("Invalid control character '%c'", *src);
                    result = BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                    goto failed;
            }
        }
    }

    *dst = 0;
    *dstString = string;
    *ptr = src + 1;
    return BugsnagKSJSON_OK;

failed:
    free(string);
    *ptr = src;
    return result;
}

int bugsnag_ksjsoncodec_i_decodeElement(const char** ptr,
                                const char* const end,
                                const char* const name,
                                BugsnagKSJSONDecodeCallbacks* const callbacks,
                                void* const userData)
{
    skipWhitespace(ptr, end);
    unlikely_if(*ptr >= end)
    {
        BugsnagKSLOG_DEBUG("Premature end of data");
        return BugsnagKSJSON_ERROR_INCOMPLETE;
    }

    int sign = 1;
    int result;

    switch(**ptr)
    {
        case '[':
        {
            (*ptr)++;
            result = callbacks->onBeginArray(name, userData);
            unlikely_if(result != BugsnagKSJSON_OK) return result;
            while(*ptr < end)
            {
                skipWhitespace(ptr, end);
                unlikely_if(**ptr == ']')
                {
                    (*ptr)++;
                    return callbacks->onEndContainer(userData);
                }
                result = bugsnag_ksjsoncodec_i_decodeElement(ptr,
                                                     end,
                                                     NULL,
                                                     callbacks,
                                                     userData);
                unlikely_if(result != BugsnagKSJSON_OK) return result;
                skipWhitespace(ptr, end);
                likely_if(**ptr == ',')
                {
                    (*ptr)++;
                }
            }
            BugsnagKSLOG_DEBUG("Premature end of data");
            return BugsnagKSJSON_ERROR_INCOMPLETE;
        }
        case '{':
        {
            (*ptr)++;
            result = callbacks->onBeginObject(name, userData);
            unlikely_if(result != BugsnagKSJSON_OK) return result;
            while(*ptr < end)
            {
                skipWhitespace(ptr, end);
                unlikely_if(**ptr == '}')
                {
                    (*ptr)++;
                    return callbacks->onEndContainer(userData);
                }
                char* key;
                result = bugsnag_ksjsoncodec_i_decodeString(ptr, end, &key);
                unlikely_if(result != BugsnagKSJSON_OK) return result;
                skipWhitespace(ptr, end);
                unlikely_if(**ptr != ':')
                {
                    free(key);
                    BugsnagKSLOG_DEBUG("Expected ':' but got '%c'", **ptr);
                    return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
                }
                (*ptr)++;
                skipWhitespace(ptr, end);
                result = bugsnag_ksjsoncodec_i_decodeElement(ptr,
                                                     end,
                                                     key,
                                                     callbacks,
                                                     userData);
                free(key);
                unlikely_if(result != BugsnagKSJSON_OK) return result;
                skipWhitespace(ptr, end);
                likely_if(**ptr == ',')
                {
                    (*ptr)++;
                }
            }
            BugsnagKSLOG_DEBUG("Premature end of data");
            return BugsnagKSJSON_ERROR_INCOMPLETE;
        }
        case '\"':
        {
            char* string;
            result = bugsnag_ksjsoncodec_i_decodeString(ptr, end, &string);
            unlikely_if(result != BugsnagKSJSON_OK) return result;
            result = callbacks->onStringElement(name,
                                                string,
                                                userData);
            free(string);
            return result;
        }
        case 'f':
        {
            unlikely_if(end - *ptr < 5)
            {
                BugsnagKSLOG_DEBUG("Premature end of data");
                return BugsnagKSJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!((*ptr)[1] == 'a' &&
                          (*ptr)[2] == 'l' &&
                          (*ptr)[3] == 's' &&
                          (*ptr)[4] == 'e'))
            {
                BugsnagKSLOG_DEBUG("Expected \"false\" but got \"f%c%c%c%c\"",
                            (*ptr)[1], (*ptr)[2], (*ptr)[3], (*ptr)[4]);
                return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
            }
            *ptr += 5;
            return callbacks->onBooleanElement(name, false, userData);
        }
        case 't':
        {
            unlikely_if(end - *ptr < 4)
            {
                BugsnagKSLOG_DEBUG("Premature end of data");
                return BugsnagKSJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!((*ptr)[1] == 'r' &&
                          (*ptr)[2] == 'u' &&
                          (*ptr)[3] == 'e'))
            {
                BugsnagKSLOG_DEBUG("Expected \"true\" but got \"t%c%c%c\"",
                            (*ptr)[1], (*ptr)[2], (*ptr)[3]);
                return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
            }
            *ptr += 4;
            return callbacks->onBooleanElement(name, true, userData);
        }
        case 'n':
        {
            unlikely_if(end - *ptr < 4)
            {
                BugsnagKSLOG_DEBUG("Premature end of data");
                return BugsnagKSJSON_ERROR_INCOMPLETE;
            }
            unlikely_if(!((*ptr)[1] == 'u' &&
                          (*ptr)[2] == 'l' &&
                          (*ptr)[3] == 'l'))
            {
                BugsnagKSLOG_DEBUG("Expected \"null\" but got \"n%c%c%c\"",
                            (*ptr)[1], (*ptr)[2], (*ptr)[3]);
                return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
            }
            *ptr += 4;
            return callbacks->onNullElement(name, userData);
        }
        case '-':
            sign = -1;
            (*ptr)++;
            unlikely_if(!isdigit(**ptr))
        {
            BugsnagKSLOG_DEBUG("Not a digit: '%c'", **ptr);
            return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
        }
            // Fall through
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        {
            // Try integer conversion.
            long long accum = 0;
            const char* const start = *ptr;

            for(; *ptr < end && isdigit(**ptr); (*ptr)++)
            {
                accum = accum * 10 + (**ptr - '0');
                unlikely_if(accum < 0)
                {
                    // Overflow
                    break;
                }
            }

            unlikely_if(*ptr >= end)
            {
                BugsnagKSLOG_DEBUG("Premature end of data");
                return BugsnagKSJSON_ERROR_INCOMPLETE;
            }

            if(!isFPChar(**ptr) && accum >= 0)
            {
                accum *= sign;
                return callbacks->onIntegerElement(name, accum, userData);
            }

            while(*ptr < end && isFPChar(**ptr))
            {
                (*ptr)++;
            }

            unlikely_if(*ptr >= end)
            {
                BugsnagKSLOG_DEBUG("Premature end of data");
                return BugsnagKSJSON_ERROR_INCOMPLETE;
            }

            // our buffer is not necessarily NULL-terminated, so
            // it would be undefined to call sscanf/sttod etc. directly.
            // instead we create a temporary string.
            double value;
            size_t len = (size_t)(*ptr - start);
            char * buf = malloc(len + 1);
            strncpy(buf, start, len);
            buf[len] = '\0';

            sscanf(buf, "%lg", &value);

            free(buf);

            value *= sign;
            return callbacks->onFloatingPointElement(name, value, userData);
        }
    }
    BugsnagKSLOG_DEBUG("Invalid character '%c'", **ptr);
    return BugsnagKSJSON_ERROR_INVALID_CHARACTER;
}

int bugsnag_ksjson_decode(const char* const data,
                  size_t length,
                  BugsnagKSJSONDecodeCallbacks* const callbacks,
                  void* const userData,
                  size_t* const errorOffset)
{
    const char* ptr = data;

    int result = bugsnag_ksjsoncodec_i_decodeElement(&ptr,
                                             ptr + length,
                                             NULL,
                                             callbacks,
                                             userData);
    likely_if(result == BugsnagKSJSON_OK)
    {
        result = callbacks->onEndData(userData);
    }

    unlikely_if(result != BugsnagKSJSON_OK && errorOffset != NULL)
    {
        *errorOffset = (size_t)(ptr - data);
    }
    return result;
}
