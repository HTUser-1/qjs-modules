#include <cutils.h>
#include "property-enumeration.h"
#include <quickjs.h>
#include "utils.h"
#include "vector.h"
#include "buffer-utils.h"
#include "location.h"

#include <stdint.h>

char* js_inspect_tostring(JSContext* ctx, JSValueConst value);

/**
 * \defgroup quickjs-xml QuickJS module: xml - XML parser & printer
 * @{
 */
#define WS 0x01
#define START 0x02
#define END 0x04
#define QUOTE 0x08
#define CLOSE 0x10
#define EQUAL 0x20
#define SPECIAL 0x40
#define SLASH 0x80
#define BACKSLASH 0x100
#define QUESTION 0x200
#define EXCLAM 0x400
#define HYPHEN 0x400

static int chars[256] = {0};

typedef struct {
  uint32_t idx;
  JSValue obj;
  const uint8_t* name;
  size_t namelen;
} OutputValue;

void
character_classes_init(int c[256]) {
  c[' '] = WS;
  c['\t'] = WS;
  c['\r'] = WS;
  c['\n'] = WS;
  c['!'] = SPECIAL | EXCLAM;
  c['"'] = QUOTE;
  c['/'] = END | SLASH;
  c['<'] = START;
  c['='] = EQUAL;
  c['>'] = END | CLOSE;
  c['?'] = SPECIAL | QUESTION;
  c['\\'] = BACKSLASH;
  c['-'] = HYPHEN;
}

#define yield_push() \
  do { \
    /*printf("push[%zu] %.*s\n", vector_size(&st, sizeof(OutputValue)), (int)namelen, name);*/ \
    out = vector_push(&st, ((OutputValue){0, JS_NewArray(ctx), name, namelen})); \
    JS_SetPropertyStr(ctx, element, "children", out->obj); \
  } while(0)

#define yield_pop() \
  do { \
    OutputValue* top = vector_back(&st, sizeof(OutputValue)); \
    /*printf("pop %.*s\n", (int)top->namelen, top->name);*/ \
    if(vector_size(&st, sizeof(OutputValue)) >= 2 ?) { \
      vector_pop(&st, sizeof(OutputValue)); \
      out = vector_back(&st, sizeof(OutputValue))); \
    } \
    while(0)

#define yield_next() \
  do { \
    element = JS_NewObject(ctx); \
    JS_SetPropertyUint32(ctx, out->obj, out->idx++, element); \
  } while(0)

#define yield_return(index) \
  do { \
    if(index >= 1) { \
      /*printf("shrink[%zu] %zd\n",  index, vector_size(&st, sizeof(OutputValue)) - index);*/ \
      vector_shrink(&st, sizeof(OutputValue), index); \
      out = vector_back(&st, sizeof(OutputValue)); \
    } \
  } while(0)

#define parse_getc() ((c = *++ptr), ptr >= end ? done = TRUE : 0)
#define parse_skip(cond) \
  do { \
    c = *ptr; \
    if(!(cond)) \
      break; \
    if(++ptr >= end) \
      done = TRUE; \
  } while(!done)

#define parse_until(cond) parse_skip(!(cond))
#define parse_skipspace() parse_skip(chars[c] & WS)
#define parse_is(c, classes) (chars[(c)] & (classes))

static int32_t
find_tag(Vector* st, const char* name, size_t namelen) {
  OutputValue* o;
  int32_t index = vector_size(st, sizeof(OutputValue));

  for(o = vector_back(st, sizeof(OutputValue)); o >= (OutputValue*)vector_begin(st); o--) {
    --index;

    if(o->namelen == namelen && !strncmp((const char*)o->name, name, namelen))
      return index;
  }
  return -1;
}

static int32_t
xml_num_children(JSContext* ctx, JSValueConst element) {
  int64_t num_children = -1;
  JSValue children = JS_GetPropertyStr(ctx, element, "children");

  if(JS_IsArray(ctx, children))
    num_children = js_array_length(ctx, children);

  JS_FreeValue(ctx, children);
  return num_children;
}

static void
xml_set_attr_value(JSContext* ctx, JSValueConst obj, const char* attr, size_t alen, JSValue value) {
  JSAtom prop;
  prop = JS_NewAtomLen(ctx, (const char*)attr, alen);
  JS_SetProperty(ctx, obj, prop, value);
  JS_FreeAtom(ctx, prop);
}

static void
xml_set_attr_bytes(JSContext* ctx, JSValueConst obj, const char* attr, size_t alen, const uint8_t* str, size_t slen) {
  JSValue value;
  value = JS_NewStringLen(ctx, (const char*)str, slen);
  xml_set_attr_value(ctx, obj, attr, alen, value);
  // JS_FreeValue(ctx, value);
}

static void
xml_write_attributes(JSContext* ctx, JSValueConst attributes, DynBuf* db) {
  size_t i;
  PropertyEnumeration props = {0};

  property_enumeration_init(&props, ctx, JS_DupValue(ctx, attributes), PROPENUM_DEFAULT_FLAGS);

  for(i = 0; i < props.tab_atom_len; i++) {
    const char *keystr, *valuestr;
    JSValue value;
    property_enumeration_setpos(&props, i);
    keystr = property_enumeration_keystr(&props, ctx);
    value = property_enumeration_value(&props, ctx);
    dbuf_putc(db, ' ');
    dbuf_putstr(db, keystr);
    if(!(JS_IsBool(value) && JS_ToBool(ctx, value))) {
      valuestr = property_enumeration_valuestr(&props, ctx);
      dbuf_putstr(db, "=\"");
      dbuf_putstr(db, valuestr);
      js_cstring_free(ctx, valuestr);
      dbuf_putc(db, '"');
    }
    js_cstring_free(ctx, keystr);
    JS_FreeValue(ctx, value);
  }

  property_enumeration_reset(&props, JS_GetRuntime(ctx));
}

static inline void
xml_write_indent(DynBuf* db, int32_t depth) {
  while(depth-- > 0) dbuf_putstr(db, "  ");
}

static void
xml_write_string(JSContext* ctx, const char* textStr, size_t textLen, DynBuf* db, int32_t depth) {
  const char* p;
  for(p = textStr;;) {
    size_t n;

    while(textLen) {
      if(byte_chr("\r\n \t", 4, *p) == 4)
        break;
      p++;
      textLen--;
    }
    n = byte_chr(p, textLen, '\n');
    dbuf_append(db, (const uint8_t*)p, n);
    if(n < textLen)
      n++;
    p += n;
    textLen -= n;

    /*while(textLen > 0 &&  (p[0] == ' ' ||  p[0] == '\t')) {
      textLen--;
      p++;
    }    */

    if(textLen == 0)
      break;

    if(depth > 0) {
      dbuf_putc(db, '\n');
      xml_write_indent(db, depth + 1);
    }
  }
}

static void
xml_write_text(JSContext* ctx, JSValueConst text, DynBuf* db, int32_t depth, BOOL multiline) {
  const char* textStr;
  size_t textLen;
  textStr = JS_ToCStringLen(ctx, &textLen, text);
  if(multiline)
    xml_write_indent(db, depth);
  else {
    while(db->size > 0 && is_whitespace_char(db->buf[db->size - 1])) db->size--;
  }
  xml_write_string(ctx, textStr, textLen, db, multiline ? depth : 0);
  js_cstring_free(ctx, textStr);
  if(multiline)
    dbuf_putc(db, '\n');
}

static void
xml_write_element(JSContext* ctx, JSValueConst element, DynBuf* db, int32_t depth, BOOL self_closing) {
  JSValue attributes = JS_GetPropertyStr(ctx, element, "attributes");
  int32_t num_children;
  size_t tagLen;
  const char* tagName = js_get_propertystr_cstringlen(ctx, element, "tagName", &tagLen);
  BOOL isComment;

  if(!tagName || !tagName[0])
    return;

  assert(tagName);
  isComment = !strncmp(tagName, "!--", 3);

  if(depth > 0)
    xml_write_indent(db, depth);

  if(tagName[0])
    dbuf_putc(db, '<');

  if(isComment) {
    if(TRUE || byte_chr(tagName, tagLen, '\n') < tagLen) {
      /*xml_write_string(ctx, tagName, tagLen - 2, db, depth - 1);
      dbuf_putc(db, '\n');
      xml_write_indent(db, depth + 1);
      dbuf_putc(db, '-');
      dbuf_putc(db, '-');*/
      dbuf_put(db, tagName, tagLen);
    } else {
      xml_write_string(ctx, tagName, tagLen, db, depth - 1);
    }
  } else if(tagName[0] == '!') {
    dbuf_putstr(db, tagName);
  } else {
    dbuf_putstr(db, tagName);
    if(JS_IsObject(attributes))
      xml_write_attributes(ctx, attributes, db);
  }
  num_children = xml_num_children(ctx, element);

  //     (tagName[0] == '/' || num_children == -1) ?
  if(tagName[0])
    dbuf_putstr(db,
                tagName[0] == '?'                                                                                    ? "?>"
                : (self_closing /*|| num_children == -1*/) && !(tagName[0] == '!' || num_children >= 0 || isComment) ? " />"
                                                                                                                     : ">");
  dbuf_putc(db, '\n');

  js_cstring_free(ctx, tagName);
  JS_FreeValue(ctx, attributes);
}

static void
xml_close_element(JSContext* ctx, JSValueConst element, DynBuf* db, int32_t depth) {
  int32_t num_children = xml_num_children(ctx, element);

  if(num_children >= 0) {
    size_t tagLen;
    const char* tagName = js_get_propertystr_cstringlen(ctx, element, "tagName", &tagLen);

    if(tagName[0] != '?' && tagName[0]) {
      if(db->size > 0 && db->buf[db->size - 1] == '\n')
        xml_write_indent(db, depth);

      dbuf_putstr(db, "</");
      dbuf_append(db, (const uint8_t*)tagName, tagLen);
      dbuf_putstr(db, ">");
      dbuf_putc(db, '\n');
    }
    js_cstring_free(ctx, tagName);
  }
}

static PropertyEnumeration*
xml_enumeration_next(Vector* vec, JSContext* ctx, DynBuf* db, int32_t max_depth) {
  PropertyEnumeration* it;
  JSValue value = JS_UNDEFINED, children;

  it = vector_back(vec, sizeof(PropertyEnumeration));
  value = property_enumeration_value(it, ctx);

  if(JS_IsObject(value)) {
    children = JS_GetPropertyStr(ctx, value, "children");
    JS_FreeValue(ctx, value);
    if(!JS_IsUndefined(children) && (max_depth == INT32_MAX || vector_size(vec, sizeof(PropertyEnumeration)) < (uint32_t)max_depth)) {
      if((it = property_enumeration_push(vec, ctx, children, PROPENUM_DEFAULT_FLAGS)))
        if(property_enumeration_setpos(it, 0))
          return it;
    }
  } else {
    JS_FreeValue(ctx, value);
  }

  for(;;) {
    if(property_enumeration_setpos(it, it->idx + 1))
      break;

    if((it = property_enumeration_pop(vec, ctx)) == 0)
      break;

    value = property_enumeration_value(it, ctx);
    {
      int32_t depth = vector_size(vec, sizeof(PropertyEnumeration)) - 1;
      depth = MAX_NUM(0, depth - 1);
      xml_close_element(ctx, value, db, depth);
    }
    JS_FreeValue(ctx, value);
  }

  return it;
}

static JSValue
js_xml_parse(JSContext* ctx, const uint8_t* buf, size_t len, const char* input_name, BOOL flat) {
  BOOL done = FALSE;
  const uint8_t *ptr, *end, *start;
  uint8_t c;
  OutputValue* out;
  JSValue ret, element = JS_UNDEFINED;
  Vector st = VECTOR(ctx);
  Location loc = {JS_NewAtom(ctx, input_name)};
  ptr = buf;
  end = buf + len;

  ret = JS_NewArray(ctx);

  out = vector_emplace(&st, sizeof(OutputValue));
  out->obj = ret;
  out->idx = 0;

  while(!done) {
    // parse_skipspace();
    start = ptr;
    parse_until(parse_is(c, START));

    if(ptr > start) {
      size_t len;

      while(start < ptr && is_whitespace_char(*start)) start++;

      len = ptr - start;
      while(len > 0 && is_whitespace_char(start[len - 1])) len--;

      if(len > 0) {
        JSValue str = JS_NewStringLen(ctx, (const char*)start, len);
        JS_SetPropertyUint32(ctx, out->obj, out->idx++, str);
      }
    }

    if(done)
      break;

    if(parse_is(c, START)) {
      const uint8_t* name;
      size_t namelen;
      BOOL closing = FALSE, self_closing = FALSE;
      parse_getc();
      if(parse_is(c, SLASH)) {
        closing = TRUE;
        parse_getc();
      }
      name = ptr;
      parse_until(parse_is(c, WS | END));
      namelen = ptr - name;

      if(closing) {
        int32_t index;
        parse_skipspace();
        if(parse_is(c, CLOSE))
          parse_getc();

        // printf("end-of [%zd] tagName: %s%.*s\n", index - 1, closing ? "/" : "", namelen, name);

        if(flat) {
          yield_next();
          xml_set_attr_bytes(ctx, element, "tagName", 7, name - 1, namelen + 1);

        } else {

          if((index = find_tag(&st, (const char*)name, namelen)) == -1) {
            char* file;
            JS_FreeValue(ctx, ret);
            location_count(&loc, (const char*)buf, start - buf);
            file = location_file(&loc, ctx);
            // printf("mismatch </%.*s> at %s:%u:%u", (int)namelen, name, file, loc.line, loc.column);
            ret = JS_ThrowSyntaxError(ctx, "mismatch </%.*s> at %s:%u:%u", (int)namelen, name, file, loc.line, loc.column);
            if(file)
              js_free(ctx, file);
            return ret;
          }

          yield_return(index);
        }
        /*continue;*/

        /* if(out->namelen == namelen && !memcmp(out->name, name, namelen)) {
           yield_pop();
           continue;
         }*/
      } else {
        // printf("element [%zd] tagName: %s%.*s\n", vector_size(&st, sizeof(OutputValue)) - 1, closing ? "/" : "", namelen, name);
        /*  printf("parent tagName: %.*s\n", out->namelen, out->name);*/
        yield_next();

        if(namelen && (parse_is(name[0], (/*QUESTION | */ EXCLAM)))) {
          self_closing = TRUE;
        }

        if(namelen >= 3 && parse_is(name[0], EXCLAM) && parse_is(name[1], HYPHEN) && parse_is(name[2], HYPHEN)) {
          /*  parse_getc();
            parse_getc();*/
          while(!done) {
            parse_getc();
            if(end - ptr >= 3 && parse_is(ptr[0], HYPHEN) && parse_is(ptr[1], HYPHEN) && parse_is(ptr[2], CLOSE)) {
              ptr += 2;
              break;
            }
          }
          namelen = ptr - name;

        } else if(namelen && parse_is(name[0], EXCLAM)) {
          parse_until(parse_is(c, CLOSE));
          namelen = ptr - name;
        }
        xml_set_attr_bytes(ctx, element, "tagName", 7, name, namelen);

        if(namelen && parse_is(name[0], EXCLAM)) {
          parse_getc();
          continue;
        }

        /* Parse attributes if not a closing tag */
        const uint8_t *attr, *value;
        size_t alen, vlen, num_attrs = 0;
        JSValue attributes = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, element, "attributes", attributes);
        while(!done) {
          parse_skipspace();
          if(parse_is(c, END))
            break;
          attr = ptr;
          parse_until(parse_is(c, EQUAL | WS | SPECIAL | CLOSE));
          if((alen = ptr - attr) == 0)
            break;
          if(parse_is(c, WS | CLOSE | SLASH)) {
            xml_set_attr_value(ctx, attributes, (const char*)attr, alen, JS_NewBool(ctx, TRUE));
            num_attrs++;
            continue;
          }
          if(parse_is(c, EQUAL)) {
            parse_getc();
            if(parse_is(c, QUOTE))
              parse_getc();
            value = ptr;
            parse_until(parse_is(c, QUOTE));
            vlen = ptr - value;
            if(parse_is(c, QUOTE))
              parse_getc();
            xml_set_attr_bytes(ctx, attributes, (const char*)attr, alen, value, vlen);
            num_attrs++;
          }
        }

        if(parse_is(name[0], QUESTION | EXCLAM)) {
          if(chars[c] == chars[name[0]]) {
            parse_getc();
            if(!flat)
              yield_push();
          }

        } else if(parse_is(c, SLASH)) {
          self_closing = TRUE;
          parse_getc();

        } else if(!self_closing) {
          /*
            out = vector_emplace(&st, sizeof(OutputValue));
               out->obj = JS_NewArray(ctx);
               out->idx = 0;
               out->name = name;
               out->namelen = namelen;
               JS_SetPropertyStr(ctx, element, "children", out->obj);*/
          if(!flat)
            yield_push();
        }
        /*{
          char* estr = (char*)js_inspect_tostring(ctx, element);
          printf("%03zu element #%i %s\n", vector_size(&st, sizeof(OutputValue)), out->idx, estr);
          js_free(ctx, estr);
        }*/
      }

      if(self_closing) {
        char* tagName = js_mallocz(ctx, namelen + 2);
        tagName[0] = '/';
        strncpy(&tagName[1], (const char*)name, namelen);
        tagName[namelen + 1] = '\0';

        yield_next();
        xml_set_attr_bytes(ctx, element, "tagName", 7, (const uint8_t*)tagName, namelen + 1);
        js_free(ctx, tagName);
      }

      parse_skipspace();
      if(parse_is(c, CLOSE))
        parse_getc();
    }
  }
  JS_FreeAtom(ctx, loc.file);
  return ret;
}

static JSValue
js_xml_read(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  JSValue ret;
  InputBuffer input = js_input_chars(ctx, argv[0]);
  const char* input_name = 0;
  BOOL flat = FALSE;

  if(input.data == 0 || input.size == 0) {
    JS_ThrowReferenceError(ctx, "xml.read(): expecting buffer or string");
    return JS_EXCEPTION;
  }

  if(argc >= 2)
    input_name = JS_ToCString(ctx, argv[1]);

  if(argc >= 3) {
    if(JS_IsObject(argv[2]))
      flat = js_get_propertystr_bool(ctx, argv[2], "flat");
    else if(JS_IsBool(argv[2]))
      flat = JS_ToBool(ctx, argv[2]);
  }

  ret = js_xml_parse(ctx, input.data, input.size, input_name ? input_name : "<input>", flat);

  if(input_name)
    JS_FreeCString(ctx, input_name);

  input_buffer_free(&input, ctx);
  return ret;
}

static JSValue
js_xml_write_tree(JSContext* ctx, JSValueConst obj, int max_depth, DynBuf* output) {
  Vector enumerations = VECTOR(ctx);
  PropertyEnumeration* it;
  JSValue str, value = JS_UNDEFINED;

  it = property_enumeration_push(&enumerations, ctx, JS_DupValue(ctx, obj), PROPENUM_DEFAULT_FLAGS);

  do {
    int32_t depth = vector_size(&enumerations, sizeof(PropertyEnumeration)) - 1;

    depth = MAX_NUM(0, depth - 1);

    value = property_enumeration_value(it, ctx);

    if(JS_IsString(value)) {
      xml_write_text(ctx, value, output, depth, it->tab_atom_len > 1);
    } else if(JS_IsObject(value) && !JS_IsArray(ctx, value)) {
      int32_t num_children = xml_num_children(ctx, value);
      xml_write_element(ctx, value, output, depth, num_children == -1);
    }

    JS_FreeValue(ctx, value);
  } while((it = xml_enumeration_next(&enumerations, ctx, output, max_depth)));

  while(output->size > 0 && (output->buf[output->size - 1] == '\0' || byte_chr("\r\n\t ", 4, output->buf[output->size - 1]) < 4))
    output->size--;
  dbuf_putc(output, '\0');

  str = JS_NewString(ctx, (const char*)output->buf);
  // str = JS_NewStringLen(ctx, output->buf, output->size);

  vector_foreach_t(&enumerations, it) { property_enumeration_reset(it, JS_GetRuntime(ctx)); }
  vector_free(&enumerations);
  return str;
}

static JSValue
js_xml_write_list(JSContext* ctx, JSValueConst obj, size_t len, DynBuf* output) {
  size_t i;
  int32_t depth = 0;
  BOOL single_line = FALSE;
  const char *tagName = 0, *nextTag;

  JSValue value = JS_UNDEFINED, next = JS_GetPropertyUint32(ctx, obj, 0);
  nextTag = JS_IsObject(next) ? js_get_propertystr_cstring(ctx, next, "tagName") : 0;

  for(i = 0; i < len; i++) {
    JS_FreeValue(ctx, value);
    value = next;
    next = JS_GetPropertyUint32(ctx, obj, i + 1);

    tagName = nextTag;
    nextTag = JS_IsObject(next) ? js_get_propertystr_cstring(ctx, next, "tagName") : 0;

    if(JS_IsString(value)) {
      const char* s = JS_ToCString(ctx, value);
      uint32_t newlines = str_count(s, '\n');
      JS_FreeCString(ctx, s);
      single_line = newlines == 0;

      xml_write_text(ctx, value, output, depth, !single_line);

    } else if(JS_IsObject(value) && !JS_IsArray(ctx, value)) {
      const char* tagName = js_get_propertystr_cstring(ctx, value, "tagName");
      BOOL self_closing = nextTag && nextTag[0] == '/' && !strcmp(tagName, &nextTag[1]);

      if(tagName[0] == '/')
        depth--;

      xml_write_element(ctx, value, output, single_line ? 0 : depth, self_closing);

      if(self_closing)
        next = JS_GetPropertyUint32(ctx, obj, ++i + 1);
      else if(tagName[0] != '/' && tagName[0] != '?' && tagName[0] != '!' && !strcasecmp(tagName, "dt"))
        depth++;

      //  JS_FreeCString(ctx, tagName);
      single_line = FALSE;
    }
    if(tagName)
      JS_FreeCString(ctx, tagName);
  }
  return JS_NewStringLen(ctx, (const char*)output->buf, output->size);
}

static JSValue
js_xml_write(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst argv[]) {
  DynBuf output = {0};
  JSValueConst obj = argc > 0 ? argv[0] : JS_UNDEFINED;
  JSValue ret, last, children = JS_UNDEFINED, arr = JS_UNDEFINED;
  int32_t max_depth = INT32_MAX;
  size_t len;
  BOOL flat = TRUE;

  js_dbuf_init(ctx, &output);

  if(argc >= 2)
    JS_ToInt32(ctx, &max_depth, argv[1]);

  if(!JS_IsArray(ctx, obj)) {
    arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, obj));
    obj = arr;
  }

  len = js_array_length(ctx, obj);

  last = JS_GetPropertyUint32(ctx, obj, len - 1);

  if(JS_IsObject(last)) {
    children = JS_GetPropertyStr(ctx, last, "children");

    if(JS_IsArray(ctx, children))
      flat = FALSE;
  }

  // printf("js_xml_write len=%zu, children=%s\n", len, JS_ToCString(ctx, children));

  if(flat)
    ret = js_xml_write_list(ctx, obj, len, &output);

  else
    ret = js_xml_write_tree(ctx, obj, max_depth, &output);

  dbuf_free(&output);

  if(!JS_IsUndefined(arr))
    JS_FreeValue(ctx, arr);

  return ret;
}

static const JSCFunctionListEntry js_xml_funcs[] = {
    JS_CFUNC_DEF("read", 1, js_xml_read),
    JS_CFUNC_DEF("write", 2, js_xml_write),
};

static int
js_xml_init(JSContext* ctx, JSModuleDef* m) {

  character_classes_init(chars);

  return JS_SetModuleExportList(ctx, m, js_xml_funcs, countof(js_xml_funcs));
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_xml
#endif

VISIBLE JSModuleDef*
JS_INIT_MODULE(JSContext* ctx, const char* module_name) {
  JSModuleDef* m;
  m = JS_NewCModule(ctx, module_name, js_xml_init);
  if(!m)
    return NULL;
  JS_AddModuleExportList(ctx, m, js_xml_funcs, countof(js_xml_funcs));
  return m;
}
