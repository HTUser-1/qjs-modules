#include "defines.h"
#include "quickjs-textcode.h"
#include "utils.h"
#include "buffer-utils.h"
#include "debug.h"
#include <libutf.h>
#include <libutf.h>
#include "tutf8e/include/tutf8e.h"

/**
 * \addtogroup quickjs-textdecoder
 * @{
 */

thread_local VISIBLE JSClassID js_decoder_class_id = 0, js_encoder_class_id = 0;
thread_local JSValue textdecoder_proto = {{0}, JS_TAG_UNDEFINED}, textdecoder_ctor = {{0}, JS_TAG_UNDEFINED}, textencoder_proto = {{0}, JS_TAG_UNDEFINED},
                     textencoder_ctor = {{0}, JS_TAG_UNDEFINED};

const TUTF8encoder* tutf8e_coders[] = {
    /* 0, 0, 0, 0, 0, 0, 0, 0, */
    &tutf8e_encoder_iso_8859_1,   &tutf8e_encoder_iso_8859_2,   &tutf8e_encoder_iso_8859_3,   &tutf8e_encoder_iso_8859_4,   &tutf8e_encoder_iso_8859_5,
    &tutf8e_encoder_iso_8859_6,   &tutf8e_encoder_iso_8859_7,   &tutf8e_encoder_iso_8859_8,   &tutf8e_encoder_iso_8859_9,   &tutf8e_encoder_iso_8859_10,
    &tutf8e_encoder_iso_8859_11,  &tutf8e_encoder_iso_8859_13,  &tutf8e_encoder_iso_8859_14,  &tutf8e_encoder_iso_8859_15,  &tutf8e_encoder_iso_8859_16,
    &tutf8e_encoder_windows_1250, &tutf8e_encoder_windows_1251, &tutf8e_encoder_windows_1252, &tutf8e_encoder_windows_1253, &tutf8e_encoder_windows_1254,
    &tutf8e_encoder_windows_1255, &tutf8e_encoder_windows_1256, &tutf8e_encoder_windows_1257, &tutf8e_encoder_windows_1258,
};

const char* const textcode_encodings[] = {
    "unknown",      "UTF-8",        "UTF-16",       "UTF-32",       "unknown",      "UTF-8",        "UTF-16BE",     "UTF-32BE",
    "ISO-8859-1",   "ISO-8859-2",   "ISO-8859-3",   "ISO-8859-4",   "ISO-8859-5",   "ISO-8859-6",   "ISO-8859-7",   "ISO-8859-8",
    "ISO-8859-9",   "ISO-8859-10",  "ISO-8859-11",  "ISO-8859-13",  "ISO-8859-14",  "ISO-8859-15",  "ISO-8859-16",  "WINDOWS-1250",
    "WINDOWS-1251", "WINDOWS-1252", "WINDOWS-1253", "WINDOWS-1254", "WINDOWS-1255", "WINDOWS-1256", "WINDOWS-1257", "WINDOWS-1258",
};

enum {
  DECODER_DECODE,
  DECODER_END,
};
enum {
  DECODER_ENCODING,
  DECODER_ENDIANNESS,
  DECODER_BUFFERED,
};

static size_t
textdecoder_try(const void* in, size_t len) {
  const uint8_t *x, *y, *end;
  size_t r = 0;
  int ch;
  x = in;
  end = x + len;
  while(x < end) {
    y = x;
    if((ch = unicode_from_utf8(x, end - x, &x)) == -1)
      break;
    r += x - y;
  }
  return r;
}

size_t
textdecoder_length(TextDecoder* td) {
  size_t len = 0, r;

  r = textdecoder_try(ringbuffer_begin(&td->buffer), ringbuffer_continuous(&td->buffer));
  len += r;

  if(len == ringbuffer_continuous(&td->buffer))
    if(td->buffer.head < td->buffer.tail) {
      r = textdecoder_try(td->buffer.data, ringbuffer_head(&td->buffer));
      len += r;
    }

  return len;
}

JSValue
textdecoder_decode(TextDecoder* dec, JSContext* ctx) {
  JSValue ret = JS_UNDEFINED;
  DynBuf dbuf;
  size_t i = 0, blen;
  uint_least32_t cp;
  char tmp[UTF8_CHAR_LEN_MAX];
  int len = 0;
  js_dbuf_init(ctx, &dbuf);
  blen = ringbuffer_length(&dec->buffer);

  if(blen)
    switch(dec->encoding) {
      case UTF8: {
        size_t blen, rlen = ringbuffer_length(&dec->buffer);

        if((blen = textdecoder_length(dec)) < rlen) {
          ringbuffer_normalize(&dec->buffer);
          blen = textdecoder_length(dec);
        }

        ret = JS_NewStringLen(ctx, (const char*)ringbuffer_begin(&dec->buffer), blen);
        ringbuffer_skip(&dec->buffer, blen);
        break;
      }
      case UTF16: {
        uint_least16_t* ptr = ringbuffer_begin(&dec->buffer);
        size_t n = blen & ~(0x1);

        for(i = 0; i < n; ptr = ringbuffer_next(&dec->buffer, ptr), i += 2) {
          uint_least16_t u16[2] = {uint16_get_endian(ptr, dec->endian), 0};
          size_t ns = 2;

          if(utf16_multiword(u16)) {
            if(i + 2 >= n)
              break;
            u16[1] = uint16_get_endian(ptr + 1, dec->endian == BIG);
            ns += 2;
          }

          if(!libutf_c16_to_c32(u16, &cp)) {
            ret = JS_ThrowInternalError(ctx, "%s: TextDecoder: not a valid utf-16 code at (%zu: 0x%04x, 0x%04x): %" PRIu32, __func__, i, ptr[0], ptr[1], cp);
            break;
          }
          len = unicode_to_utf8((void*)tmp, cp);

          if(dbuf_put(&dbuf, (const void*)tmp, len))
            return JS_ThrowOutOfMemory(ctx);
        }

        break;
      }
      case UTF32: {
        const uint_least32_t* ptr = ringbuffer_begin(&dec->buffer);
        size_t n = blen & ~(0x3);

        for(i = 0; i < n; ptr = ringbuffer_next(&dec->buffer, ptr), i += 4) {
          cp = uint32_get_endian(ptr, dec->endian);
          if(!libutf_c32_to_c8(cp, &len, tmp)) {
            ret = JS_ThrowInternalError(ctx, "%s: TextDecoder: not a valid utf-32 code at (%zu: 0x%04x, 0x%04x): %" PRIu32, __func__, i, ptr[0], ptr[1], cp);
            break;
          }
          if(dbuf_put(&dbuf, (const void*)tmp, len))
            return JS_ThrowOutOfMemory(ctx);
        }
        break;
      }
      default: {
        TUTF8encoder encoder;

        if((encoder = *tutf8e_coders[dec->encoding - 8])) {

          const char* ptr = ringbuffer_begin(&dec->buffer);
          size_t n = 0;

          if(TUTF8E_OK == tutf8e_encoder_buffer_length(encoder, ptr, 0, blen, &n)) {

            uint8_t* dst = dbuf_reserve(&dbuf, n);

            if(TUTF8E_OK == tutf8e_encoder_buffer_encode(encoder, ptr, blen, 0, (char*)dst, &n)) {
              dbuf.size += n;
              i = blen;
            }
          }

        } else {
          ret = JS_ThrowInternalError(ctx, "%s: TextDecoder: unknown encoding: %s", __func__, textcode_encodings[dec->encoding]);
        }
        break;
      }
    }

  ringbuffer_skip(&dec->buffer, i);

  if(JS_IsUndefined(ret) && dbuf.size > 0)
    ret = JS_NewStringLen(ctx, (const char*)dbuf.buf, dbuf.size);

  dbuf_free(&dbuf);

  return ret;
}

static JSValue
js_decoder_get(JSContext* ctx, JSValueConst this_val, int magic) {
  TextDecoder* dec;
  JSValue ret = JS_UNDEFINED;
  if(!(dec = js_decoder_data(ctx, this_val)))
    return ret;
  switch(magic) {
    case DECODER_ENCODING: {
      ret = JS_NewString(ctx, textcode_encodings[dec->type_code]);
      break;
    }
    case DECODER_ENDIANNESS: {
      ret = JS_NewBool(ctx, dec->endian == BIG);
      break;
    }
    case DECODER_BUFFERED: {
      ret = JS_NewUint32(ctx, ringbuffer_length(&dec->buffer));
      break;
    }
  }
  return ret;
}

static JSValue
js_decoder_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst argv[]) {
  JSValue obj = JS_UNDEFINED;
  JSValue proto;
  TextDecoder* dec;

  if(!(dec = js_mallocz(ctx, sizeof(TextDecoder))))
    return JS_ThrowOutOfMemory(ctx);

  /* using new_target to get the prototype is necessary when the class is extended. */
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if(JS_IsException(proto))
    goto fail;
  if(!JS_IsObject(proto))
    proto = textdecoder_proto;

  /* using new_target to get the prototype is necessary when the class is extended. */
  obj = JS_NewObjectProtoClass(ctx, proto, js_decoder_class_id);
  JS_FreeValue(ctx, proto);
  if(JS_IsException(obj))
    goto fail;

  ringbuffer_init(&dec->buffer, ctx);

  if(argc >= 1) {
    const char* s = JS_ToCString(ctx, argv[0]);

    if(s[case_finds(s, "utf32")] || s[case_finds(s, "utf-32")])
      dec->encoding = UTF32;
    else if(s[case_finds(s, "utf16")] || s[case_finds(s, "utf-16")])
      dec->encoding = UTF16;
    else if(s[case_finds(s, "utf8")] || s[case_finds(s, "utf-8")])
      dec->encoding = UTF8;
    else {
      return JS_ThrowInternalError(ctx, "%s: TextDecoder: '%s' is invalid s", __func__, s);
    }

    if(s[case_finds(s, "be")] || s[case_finds(s, "be")])
      dec->endian = BIG;

    JS_FreeCString(ctx, s);
  } else {
    dec->encoding = UTF8;
  }
  JS_SetOpaque(obj, dec);

  return obj;

fail:
  js_free(ctx, dec);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue
js_decoder_decode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  TextDecoder* dec;
  JSValue ret = JS_UNDEFINED;

  if(!(dec = js_decoder_data(ctx, this_val)))
    return JS_EXCEPTION;

  switch(magic) {
    case DECODER_END:
    case DECODER_DECODE: {
      InputBuffer in = js_input_chars(ctx, argv[0]);
      uint8_t tmp[UTF8_CHAR_LEN_MAX];
      size_t i;

      // printf("js_decoder_decode (1) %s length=%zu in.size=%zu\n", magic == DECODER_DECODE ? "decode" :
      // "end", ringbuffer_length(&dec->buffer), in.size);
      if(ringbuffer_write(&dec->buffer, in.data, in.size) < 0)
        return JS_ThrowInternalError(ctx, "%s: TextDecoder: ringbuffer %s failed", __func__, magic == DECODER_DECODE ? "decode" : "end");

      if(ringbuffer_length(&dec->buffer) == 0)
        ret = JS_NULL;
      else
        ret = textdecoder_decode(dec, ctx);

      if(magic == DECODER_END)
        ringbuffer_reset(&dec->buffer);
      break;
    }
  }
  return ret;
}

static JSValue
js_decoder_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  TextDecoder* dec;

  if(!(dec = js_decoder_data(ctx, this_val)))
    return JS_EXCEPTION;

  JSValue obj = JS_NewObjectClass(ctx, js_decoder_class_id);

  JS_DefinePropertyValueStr(ctx, obj, "encoding", JS_NewString(ctx, textcode_encodings[dec->type_code]), JS_PROP_ENUMERABLE);
  JS_DefinePropertyValueStr(ctx, obj, "buffered", JS_NewUint32(ctx, ringbuffer_length(&dec->buffer)), JS_PROP_ENUMERABLE);
  return obj;
}

static void
js_decoder_finalizer(JSRuntime* rt, JSValue val) {
  TextDecoder* dec = JS_GetOpaque(val, js_decoder_class_id);
  if(dec) {
    ringbuffer_free(&dec->buffer);
    js_free_rt(rt, dec);
  }
  // JS_FreeValueRT(rt, val);
}

static JSClassDef js_decoder_class = {
    .class_name = "TextDecoder",
    .finalizer = js_decoder_finalizer,
};

static const JSCFunctionListEntry js_decoder_funcs[] = {
    JS_CFUNC_MAGIC_DEF("decode", 1, js_decoder_decode, DECODER_DECODE),
    JS_CFUNC_MAGIC_DEF("end", 1, js_decoder_decode, DECODER_END),
    JS_CGETSET_ENUMERABLE_DEF("encoding", js_decoder_get, 0, DECODER_ENCODING),
    JS_CGETSET_MAGIC_DEF("endian", js_decoder_get, 0, DECODER_ENDIANNESS),
    JS_CGETSET_MAGIC_DEF("buffered", js_decoder_get, 0, DECODER_BUFFERED),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "TextDecoder", JS_PROP_CONFIGURABLE),
};

enum {
  ENCODER_ENCODE,
  ENCODER_END,
};
enum {
  ENCODER_ENCODING,
  ENCODER_ENDIANNESS,
  ENCODER_BUFFERED,
};

/*static size_t
textencoder_try(const void* in, size_t len) {
  const uint8_t *x, *y, *end;
  size_t r = 0;
  int ch;
  for(x = in, end=x+len; x < end; ) {
    y = x;
    if((ch = unicode_from_utf8(x, end - x, &x)) == -1)
      break;
    r += x - y;
  }
  return r;
}

size_t
textencoder_length(TextEncoder* td) {
  size_t r = 0;

  r += textencoder_try(ringbuffer_begin(&td->buffer), ringbuffer_continuous(&td->buffer));

  if(td->buffer.head < td->buffer.tail)
    r += textencoder_try(td->buffer.data, ringbuffer_head(&td->buffer));

  return r;
}*/

JSValue
textencoder_read(TextEncoder* te, JSContext* ctx) {
  JSValue ret, buf;
  int bits;
  size_t len = ringbuffer_length(&te->buffer);

  if(len > ringbuffer_continuous(&te->buffer))
    ringbuffer_normalize(&te->buffer);

  switch(te->encoding) {
    case UTF8: bits = 8; break;
    case UTF16: bits = 16; break;
    case UTF32: bits = 32; break;
    default: return JS_ThrowInternalError(ctx, "%s: TextEncoder: invalid encoding: %d", __func__, te->encoding);
  }

  buf = JS_NewArrayBufferCopy(ctx, ringbuffer_begin(&te->buffer), len);
  ret = js_typedarray_new(ctx, bits, FALSE, FALSE, buf);
  JS_FreeValue(ctx, buf);

  te->buffer.tail += len;
  return ret;
}

JSValue
textencoder_encode(TextEncoder* enc, InputBuffer in, JSContext* ctx) {
  JSValue ret = JS_UNDEFINED;
  size_t i;
  uint32_t cp;
  uint8_t u8[UTF8_CHAR_LEN_MAX];
  const uint8_t *ptr, *end, *next;

  switch(enc->encoding) {
    case UTF8: {
      if(ringbuffer_write(&enc->buffer, in.data, in.size) < 0)
        return JS_ThrowInternalError(ctx, "%s: TextEncoder: ringbuffer write failed", __func__);
      break;
    }
    case UTF16: {
      ptr = block_begin(&in.block);
      end = block_end(&in.block);

      for(i = 0; ptr < end; ptr = next, i++) {
        cp = unicode_from_utf8(ptr, end - ptr, &next);

        {
          uint_least16_t u16[2];
          int len;

          if(!libutf_c32_to_c16(cp, &len, u16))
            return JS_ThrowInternalError(ctx, "%s: TextEncoder: not a valid code point at (%zu) [%zu]: %" PRIu32, __func__, i, end - ptr, cp);

          for(int j = 0; j < len; j++)
            uint16_put_endian(u8 + j * 2, u16[j], enc->endian);

          if(ringbuffer_append(&enc->buffer, u8, len * sizeof(uint16_t), ctx) < 0)
            return JS_ThrowInternalError(ctx, "TextEncoder: ringbuffer write failed");
        }
      }

      break;
    }
    case UTF32: {
      ptr = block_begin(&in.block);
      end = block_end(&in.block);

      for(i = 0; ptr < end; ptr = next, i++) {
        cp = unicode_from_utf8(ptr, end - ptr, &next);
        if(cp == 0xffffffff)
          return JS_ThrowInternalError(ctx, "%s: TextEncoder: not a valid code point at (%zu): %" PRIu32, __func__, ptr - in.block.base, cp);
        /*cp = 0;
        if(!libutf_c8_to_c32(ptr, &cp))
          return JS_ThrowInternalError(ctx, "No a valid code point at (%zu) [%zu]: %" PRIu32, i, end - ptr,
        cp); next = ptr + libutf_c8_type(ptr); if(next == ptr) break;*/

        uint32_put_endian(u8, cp, enc->endian);

        if(ringbuffer_append(&enc->buffer, u8, sizeof(cp), ctx) < 0)
          return JS_ThrowInternalError(ctx, "%s: TextEncoder: ringbuffer write failed", __func__);
      }
      break;
    }
    default: {
      ret = JS_ThrowInternalError(ctx, "%s: TextEncoder: unknown encoding", __func__);
      break;
    }
  }

  return ret;
}

static JSValue
js_encoder_get(JSContext* ctx, JSValueConst this_val, int magic) {
  TextEncoder* enc;
  JSValue ret = JS_UNDEFINED;
  if(!(enc = js_encoder_data(ctx, this_val)))
    return ret;
  switch(magic) {
    case ENCODER_ENCODING: {
      ret = JS_NewString(ctx, textcode_encodings[enc->type_code]);
      break;
    }
    case ENCODER_ENDIANNESS: {
      ret = JS_NewBool(ctx, enc->endian == BIG);
      break;
    }
    case ENCODER_BUFFERED: {
      ret = JS_NewUint32(ctx, ringbuffer_length(&enc->buffer));
      break;
    }
  }
  return ret;
}

static JSValue
js_encoder_constructor(JSContext* ctx, JSValueConst new_target, int argc, JSValueConst argv[]) {
  JSValue obj = JS_UNDEFINED;
  JSValue proto;
  TextEncoder* enc;

  if(!(enc = js_mallocz(ctx, sizeof(TextEncoder))))
    return JS_ThrowOutOfMemory(ctx);

  /* using new_target to get the prototype is necessary when the class is extended. */
  proto = JS_GetPropertyStr(ctx, new_target, "prototype");
  if(JS_IsException(proto))
    goto fail;
  if(!JS_IsObject(proto))
    proto = textencoder_proto;

  /* using new_target to get the prototype is necessary when the class is extended. */
  obj = JS_NewObjectProtoClass(ctx, proto, js_encoder_class_id);
  JS_FreeValue(ctx, proto);
  if(JS_IsException(obj))
    goto fail;

  ringbuffer_init(&enc->buffer, ctx);

  if(argc >= 1) {
    const char* s = JS_ToCString(ctx, argv[0]);

    if(s[case_finds(s, "utf32")] || s[case_finds(s, "utf-32")])
      enc->encoding = UTF32;
    else if(s[case_finds(s, "utf16")] || s[case_finds(s, "utf-16")])
      enc->encoding = UTF16;
    else if(s[case_finds(s, "utf8")] || s[case_finds(s, "utf-8")])
      enc->encoding = UTF8;
    else {
      return JS_ThrowInternalError(ctx, "TextEncoder '%s' is invalid s", s);
    }

    if(enc->encoding > UTF8)
      if(s[case_finds(s, "be")] || s[case_finds(s, "be")])
        enc->endian = BIG;

    JS_FreeCString(ctx, s);
  } else {
    enc->encoding = UTF8;
  }
  JS_SetOpaque(obj, enc);

  return obj;

fail:
  js_free(ctx, enc);
  JS_FreeValue(ctx, obj);
  return JS_EXCEPTION;
}

static JSValue
js_encoder_encode(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[], int magic) {
  TextEncoder* enc;
  JSValue ret = JS_UNDEFINED;

  if(!(enc = js_encoder_data(ctx, this_val)))
    return JS_EXCEPTION;

  switch(magic) {
    case ENCODER_END:
    case ENCODER_ENCODE: {
      size_t i;
      InputBuffer in = js_input_chars(ctx, argv[0]);

      ret = textencoder_encode(enc, in, ctx);

      if(JS_IsException(ret))
        break;

      if(ringbuffer_length(&enc->buffer) == 0)
        ret = JS_NULL;
      else
        ret = textencoder_read(enc, ctx);

      if(magic == ENCODER_END)
        ringbuffer_reset(&enc->buffer);
      break;
    }
  }
  return ret;
}

static JSValue
js_encoder_inspect(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  TextEncoder* enc;

  if(!(enc = js_encoder_data(ctx, this_val)))
    return JS_EXCEPTION;

  JSValue obj = JS_NewObjectClass(ctx, js_encoder_class_id);

  JS_DefinePropertyValueStr(ctx, obj, "encoding", JS_NewString(ctx, textcode_encodings[enc->type_code]), JS_PROP_ENUMERABLE);
  JS_DefinePropertyValueStr(ctx, obj, "buffered", JS_NewUint32(ctx, ringbuffer_length(&enc->buffer)), JS_PROP_ENUMERABLE);
  return obj;
}

static void
js_encoder_finalizer(JSRuntime* rt, JSValue val) {
  TextEncoder* enc = JS_GetOpaque(val, js_encoder_class_id);
  if(enc) {
    ringbuffer_free(&enc->buffer);
    js_free_rt(rt, enc);
  }
  // JS_FreeValueRT(rt, val);
}

static JSClassDef js_encoder_class = {
    .class_name = "TextEncoder",
    .finalizer = js_encoder_finalizer,
};

static const JSCFunctionListEntry js_encoder_funcs[] = {
    JS_CFUNC_MAGIC_DEF("encode", 1, js_encoder_encode, ENCODER_ENCODE),
    JS_CFUNC_MAGIC_DEF("end", 1, js_encoder_encode, ENCODER_END),
    JS_CGETSET_ENUMERABLE_DEF("encoding", js_encoder_get, 0, ENCODER_ENCODING),
    JS_CGETSET_MAGIC_DEF("endian", js_encoder_get, 0, ENCODER_ENDIANNESS),
    JS_CGETSET_MAGIC_DEF("buffered", js_encoder_get, 0, ENCODER_BUFFERED),
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "TextEncoder", JS_PROP_CONFIGURABLE),
};

int
js_code_init(JSContext* ctx, JSModuleDef* m) {

  if(js_decoder_class_id == 0) {
    JS_NewClassID(&js_decoder_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_decoder_class_id, &js_decoder_class);

    textdecoder_ctor = JS_NewCFunction2(ctx, js_decoder_constructor, "TextDecoder", 1, JS_CFUNC_constructor, 0);
    textdecoder_proto = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, textdecoder_proto, js_decoder_funcs, countof(js_decoder_funcs));
    JS_SetClassProto(ctx, js_decoder_class_id, textdecoder_proto);

    JS_SetConstructor(ctx, textdecoder_ctor, textdecoder_proto);

    JS_NewClassID(&js_encoder_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_encoder_class_id, &js_encoder_class);

    textencoder_ctor = JS_NewCFunction2(ctx, js_encoder_constructor, "TextEncoder", 1, JS_CFUNC_constructor, 0);
    textencoder_proto = JS_NewObject(ctx);

    JS_SetPropertyFunctionList(ctx, textencoder_proto, js_encoder_funcs, countof(js_encoder_funcs));
    JS_SetClassProto(ctx, js_encoder_class_id, textencoder_proto);

    JS_SetConstructor(ctx, textencoder_ctor, textencoder_proto);

    // js_set_inspect_method(ctx, textdecoder_proto,
    // js_decoder_inspect);
  }

  if(m) {
    JS_SetModuleExport(ctx, m, "TextDecoder", textdecoder_ctor);
    JS_SetModuleExport(ctx, m, "TextEncoder", textencoder_ctor);

    /*  const char* module_name = JS_AtomToCString(ctx, m->module_name);

      if(!strcmp(module_name, "textdecoder"))
        JS_SetModuleExport(ctx, m, "default", textdecoder_ctor);

      JS_FreeCString(ctx, module_name);*/
  }

  return 0;
}

#ifdef JS_TEXTCODE_MODULE
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_textdecoder
#endif

VISIBLE JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;

  if((m = JS_NewCModule(ctx, module_name, js_code_init))) {
    JS_AddModuleExport(ctx, m, "TextDecoder");
    JS_AddModuleExport(ctx, m, "TextEncoder");
    /*if(!strcmp(module_name, "textdecoder"))
      JS_AddModuleExport(ctx, m, "default");*/
  }

  return m;
}

/**
 * @}
 */
