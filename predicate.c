#include "predicate.h"
#include <quickjs.h>
#include <libregexp.h>
#include <ctype.h>
#include "quickjs-predicate.h"
#include "buffer-utils.h"

/**
 * \addtogroup predicate
 * @{
 */
#define max(left, right) ((left) > (right) ? (left) : (right))
#define min(left, right) ((left) < (right) ? (left) : (right))

static size_t
utf8_to_unicode(const char* str, size_t len, Vector* out) {
  const uint8_t *p, *next, *end;

  for(p = (uint8_t*)str, end = p + len; p != end; p = next) {
    uint32_t codepoint = unicode_from_utf8(p, end - p, &next);
    vector_put(out, &codepoint, sizeof(uint32_t));
    p = next;
  }

  return vector_size(out, sizeof(uint32_t));
}

static void
predicate_inspect(JSValueConst value, JSContext* ctx, DynBuf* dbuf, Arguments* args, BOOL parens) {
  Predicate* pr;

  if(js_is_null_or_undefined(value)) {
    const char* arg = arguments_shift(args);

    dbuf_putstr(dbuf, arg);

  } else if((pr = js_predicate_data(value))) {
    if(parens)
      dbuf_putc(dbuf, '(');

    predicate_tosource(pr, ctx, dbuf, args);
    if(parens)
      dbuf_putc(dbuf, ')');
  } else {
    js_value_dump(ctx, value, dbuf);
  }
}

BOOL
predicate_is(JSValueConst value) {
  return !!js_predicate_data(value);
}

BOOL
predicate_callable(JSContext* ctx, JSValueConst value) {
  return predicate_is(value) || JS_IsFunction(ctx, value);
}

JSValue
predicate_eval(Predicate* pr, JSContext* ctx, JSArguments* args) {
  JSValue ret = JS_UNDEFINED;

  switch(pr->id) {
    case PREDICATE_TYPE: {
      int id = js_value_type(ctx, js_arguments_shift(args));
      ret = JS_NewBool(ctx, !!(id & pr->type.flags));
      break;
    }

    case PREDICATE_CHARSET: {
      InputBuffer input = js_input_chars(ctx, js_arguments_shift(args));
      if(pr->charset.chars.size == 0 && pr->charset.chars.data == 0) {
        vector_init(&pr->charset.chars, ctx);
        utf8_to_unicode(pr->charset.set, pr->charset.len, &pr->charset.chars);
      }
      ret = JS_NewInt32(ctx, 1);
      while(!input_buffer_eof(&input)) {
        uint32_t codepoint = input_buffer_getc(&input);
        ssize_t idx = vector_find(&pr->charset.chars, sizeof(uint32_t), &codepoint);
        if(idx == -1) {
          ret = JS_NewInt32(ctx, 0);
          break;
        }
      }
      input_buffer_free(&input, ctx);
      break;
    }

    case PREDICATE_STRING: {
      InputBuffer input = js_input_chars(ctx, js_arguments_shift(args));

      if(input.size >= pr->string.len) {
        if(!memcmp(input.data, pr->string.str, pr->string.len))
          ret = JS_NewInt32(ctx, 1);
      }
      break;
    }

    case PREDICATE_NOTNOT: {
      ret = JS_NewBool(ctx, !!JS_ToBool(ctx, predicate_value(ctx, pr->unary.predicate, args)));
      break;
    }

    case PREDICATE_NOT: {
      ret = JS_NewBool(ctx, !JS_ToBool(ctx, predicate_value(ctx, pr->unary.predicate, args)));
      break;
    }
    case PREDICATE_BNOT: {
      ret = JS_NewInt64(ctx, ~js_value_toint64_free(ctx, predicate_value(ctx, pr->unary.predicate, args)));
      break;
    }

    case PREDICATE_SQRT: {
      ret = JS_NewFloat64(ctx, sqrt(js_value_todouble_free(ctx, predicate_value(ctx, pr->unary.predicate, args))));
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      JSValue values[2] = {pr->binary.left, pr->binary.right};
      BOOL nullish[2] = {js_is_null_or_undefined(pr->binary.left), js_is_null_or_undefined(pr->binary.right)};
      size_t i, j = 0;
      double left, right, r;

      for(i = 0; i < 2; i++) {
        if(nullish[i]) {
          values[i] = js_arguments_shift(args);
        }
        values[i] = predicate_value(ctx, values[i], args);
      }

      JS_ToFloat64(ctx, &left, values[0]);
      JS_ToFloat64(ctx, &right, values[1]);

      switch(pr->id) {
        case PREDICATE_ADD: r = left + right; break;
        case PREDICATE_SUB: r = left - right; break;
        case PREDICATE_MUL: r = left * right; break;
        case PREDICATE_DIV: r = left / right; break;
        case PREDICATE_MOD: r = fmod(left, right); break;
        case PREDICATE_BOR: r = (uint64_t)left | (uint64_t)right; break;
        case PREDICATE_BAND: r = (uint64_t)left & (uint64_t)right; break;
        case PREDICATE_POW: r = pow(left, right); break;
        case PREDICATE_ATAN2: r = atan2(left, right); break;
        default: r = nan(""); break;
      }

      ret = JS_NewFloat64(ctx, r);
      break;
    }

    case PREDICATE_OR: {
      size_t i;

      for(i = 0; i < pr->boolean.npredicates; i++) {
        if(JS_ToBool(ctx, (ret = predicate_value(ctx, pr->boolean.predicates[i], args))))
          break;
      }
      break;
    }

    case PREDICATE_AND: {
      size_t i;

      for(i = 0; i < pr->boolean.npredicates; i++) {
        if(!JS_ToBool(ctx, (ret = predicate_value(ctx, pr->boolean.predicates[i], args))))
          break;
      }
      break;
    }

    case PREDICATE_XOR: {
      size_t i;
      int64_t r = 0;
      for(i = 0; i < pr->boolean.npredicates; i++) {
        int64_t i64 = 0;
        JS_ToInt64(ctx, &i64, predicate_value(ctx, pr->boolean.predicates[i], args));
        r ^= i64;
      }
      ret = JS_NewInt64(ctx, r);
      break;
    }

    case PREDICATE_REGEXP: {
      JSValue re = js_arguments_shift(args);
      InputBuffer input = js_input_chars(ctx, re);
      uint8_t* capture[CAPTURE_COUNT_MAX * 2];
      int capture_count = 0, result;

      if(pr->regexp.bytecode == 0)
        capture_count = predicate_regexp_compile(pr, ctx);

      result = lre_exec(capture, pr->regexp.bytecode, (uint8_t*)input.data, 0, input.size, 0, ctx);

      if(result && args->c > 1) {
        JSValue arg = js_arguments_shift(args);

        if(JS_IsFunction(ctx, arg)) {
          JSValue args[] = {predicate_regexp_capture(capture, capture_count, input.data, ctx), re};

          JS_Call(ctx, arg, JS_NULL, 2, args);

          JS_FreeValue(ctx, args[0]);

        } else if(JS_IsArray(ctx, arg)) {
          int i;
          JS_SetPropertyStr(ctx, arg, "length", JS_NewUint32(ctx, capture_count));
          for(i = 0; i < 2 * capture_count; i += 2) {
            JSValue left = JS_NULL;

            if(capture[i]) {
              left = JS_NewArray(ctx);
              JS_SetPropertyUint32(ctx, left, 0, JS_NewUint32(ctx, capture[i] - input.data));
              JS_SetPropertyUint32(ctx, left, 1, JS_NewUint32(ctx, capture[i + 1] - input.data));
            }

            JS_SetPropertyUint32(ctx, arg, i >> 1, left);
          }
        }
      }

      ret = JS_NewBool(ctx, result);
      break;
    }

    case PREDICATE_INSTANCEOF: {
      ret = JS_NewBool(ctx, JS_IsInstanceOf(ctx, js_arguments_shift(args), pr->unary.predicate));
      break;
    }

    case PREDICATE_PROTOTYPEIS: {
      JSValue proto = JS_GetPrototype(ctx, js_arguments_shift(args));

      ret = JS_NewBool(ctx, JS_VALUE_GET_OBJ(proto) == JS_VALUE_GET_OBJ(pr->unary.predicate));
      break;
    }

    case PREDICATE_EQUAL: {
      ret = JS_NewBool(ctx, js_value_equals(ctx, js_arguments_shift(args), pr->unary.predicate));
      break;
    }

    case PREDICATE_PROPERTY: {

      //   JSValue prop = JS_AtomToValue(ctx, pr->property.atom);

      JSValue obj = js_arguments_shift(args);

      if(JS_IsObject(obj)) {
        ret = JS_GetProperty(ctx, obj, pr->property.atom);

        if(!js_is_null_or_undefined(pr->property.predicate) && predicate_callable(ctx, pr->property.predicate)) {
          JSValue result = predicate_call(ctx, pr->property.predicate, 1, &ret);
          JS_FreeValue(ctx, ret);
          ret = result;
        }

      } else {
        ret = JS_ThrowTypeError(ctx, "target must be object, but is %s", js_value_typestr(ctx, obj));
      }

      /*    JSValue obj = pr->property.predicate;

          if(!prop && j < args->c)
            prop = JS_ValueToAtom(ctx, js_arguments_shift(args));

          if(js_is_falsish(obj) && j < args->c && JS_IsObject(args->v[j]))
            obj = js_arguments_shift(args);

          if(JS_GetOpaque(obj, js_predicate_class_id))
            obj = predicate_value(ctx, obj, args);
          else
            obj = JS_DupValue(ctx, obj);

         */
      break;
    }

    case PREDICATE_MEMBER: {
      JSValue obj = pr->member.object;
      JSValue member = js_arguments_shift(args);
      JSAtom atom = JS_ValueToAtom(ctx, member);
      JS_FreeValue(ctx, member);

      if(JS_HasProperty(ctx, obj, atom))
        ret = JS_GetProperty(ctx, obj, atom);
      else
        ret = JS_UNDEFINED;
      break;
    }

    case PREDICATE_SHIFT: {
      int shift = min(args->c, pr->shift.n);

      if(pr->shift.n <= args->c) {

        js_arguments_shiftn(args, pr->shift.n);
        ret = predicate_value(ctx, pr->shift.predicate, args);
      }
      break;
    }

    case PREDICATE_FUNCTION: {
      int i, nargs = pr->function.arity;
      JSValueConst argv[nargs];
      for(i = 0; i < nargs; i++) argv[i++] = js_arguments_shift(args);

      ret = JS_Call(ctx, pr->function.func, pr->function.this_val, nargs, argv);
      break;
    }
    default: {
      assert(0);
      break;
    }
  }

  return ret;
}

JSValue
predicate_call(JSContext* ctx, JSValueConst value, int argc, JSValueConst argv[]) {
  Predicate* pr;
  JSValue ret = JS_UNDEFINED;

  if((pr = js_predicate_data(value))) {
    JSArguments args = js_arguments_new(argc, argv);
    ret = predicate_eval(pr, ctx, &args);
  } else if(JS_IsFunction(ctx, value)) {
    ret = JS_Call(ctx, value, JS_UNDEFINED, argc, argv);
  }

  return ret;
}

JSValue
predicate_value(JSContext* ctx, JSValueConst value, JSArguments* args) {
  Predicate* pr;
  JSValue ret = JS_EXCEPTION;

  if((pr = js_predicate_data(value))) {
    ret = predicate_eval(pr, ctx, args);
  } else if(JS_IsFunction(ctx, value)) {
    ret = predicate_call(ctx, value, args->c, args->v);
  } else {
    ret = JS_DupValue(ctx, value);
  }

  return ret;
}

const char*
predicate_typename(const Predicate* pr) {
  return ((const char*[]){
      "type",       "charset",     "string", "notnot",   "not",    "bnot",  "sqrt",     "add", "sub", "mul",
      "div",        "mod",         "bor",    "band",     "pow",    "atan2", "or",       "and", "xor", "regexp",
      "instanceof", "prototypeis", "equal",  "property", "member", "shift", "function", 0,
  })[pr->id];
}

void
predicate_dump(const Predicate* pr, JSContext* ctx, DynBuf* dbuf) {
  const char* type = predicate_typename(pr);

  assert(type);
  dbuf_printf(dbuf, "Predicate.%s(", type);

  switch(pr->id) {
    case PREDICATE_TYPE: {
      dbuf_putstr(dbuf, "type == ");
      dbuf_bitflags(dbuf,
                    pr->type.flags,
                    ((const char* const[]){
                        "UNDEFINED",
                        "NULL",
                        "BOOL",
                        "INT",
                        "OBJECT",
                        "STRING",
                        "SYMBOL",
                        "BIG_FLOAT",
                        "BIG_INT",
                        "BIG_DECIMAL",
                        "FLOAT64",
                        "FUNCTION",
                        "ARRAY",
                    }));
      break;
    }

    case PREDICATE_CHARSET: {
      uint32_t i = 0, *p;
      dbuf_putstr(dbuf, "[ ");

      vector_foreach(&pr->charset.chars, sizeof(uint32_t), p) {
        if(i > 0)
          dbuf_putstr(dbuf, ", ");
        if(*p < 128)
          dbuf_printf(dbuf, "'%c'", (char)*p);
        else
          dbuf_printf(dbuf, *p > 0xffffff ? "'\\u%08x'" : *p > 0xffff ? "\\u%06x" : "'\\u%04x'", *p);
        i++;
      }
      dbuf_printf(dbuf, " (len = %zu) ]", pr->charset.len);
      break;
    }

    case PREDICATE_STRING: {
      dbuf_putc(dbuf, '\'');
      dbuf_put_escaped(dbuf, pr->string.str, pr->string.len);
      dbuf_putc(dbuf, '\'');
      break;
    }

    case PREDICATE_NOTNOT: dbuf_putc(dbuf, '!');

#if __GNUC__ >= 7
      __attribute__((fallthrough));
#endif
    case PREDICATE_NOT: {
      dbuf_putstr(dbuf, "!( ");
      dbuf_put_value(dbuf, ctx, pr->unary.predicate);
      dbuf_putstr(dbuf, " )");
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW: {
      size_t i;
      static const char* op_str[] = {" + ", " - ", " * ", " / ", " % ", " | ", " & ", " ** "};
      dbuf_putstr(dbuf, "(");
      dbuf_put_value(dbuf, ctx, pr->binary.left);

      dbuf_putstr(dbuf, op_str[pr->id - PREDICATE_ADD]);
      dbuf_put_value(dbuf, ctx, pr->binary.right);
      dbuf_putstr(dbuf, ")");
      break;
    }

    case PREDICATE_AND:
    case PREDICATE_OR:
    case PREDICATE_XOR: {
      size_t i;
      // dbuf_putstr(dbuf, "( ");

      for(i = 0; i < pr->boolean.npredicates; i++) {
        if(i > 0)
          dbuf_putstr(dbuf, ", "); // pr->id == PREDICATE_XOR ? " ^ " : pr->id == PREDICATE_AND ? " && " : " || ");

        dbuf_put_value(dbuf, ctx, pr->boolean.predicates[i]);
      }
      // dbuf_putstr(dbuf, " )");

      break;
    }

    case PREDICATE_REGEXP: {
      char flagbuf[16];

      dbuf_putc(dbuf, '/');
      dbuf_append(dbuf, pr->regexp.expr.source, pr->regexp.expr.len);
      dbuf_putc(dbuf, '/');
      dbuf_append(dbuf, flagbuf, regexp_flags_tostring(pr->regexp.expr.flags, flagbuf));
      dbuf_0(dbuf);
      break;
    }

    case PREDICATE_INSTANCEOF: {
      const char* name = js_function_name(ctx, pr->unary.predicate);
      dbuf_putstr(dbuf, name);
      js_cstring_free(ctx, name);
      break;
    }

    case PREDICATE_PROTOTYPEIS: {
      const char* name = js_object_tostring(ctx, pr->unary.predicate);
      dbuf_putstr(dbuf, name);
      js_cstring_free(ctx, name);
      break;
    }

    case PREDICATE_EQUAL: {
      js_value_dump(ctx, pr->unary.predicate, dbuf);
      break;
    }
    case PREDICATE_PROPERTY: {
      const char* arg = JS_AtomToCString(ctx, pr->property.atom);
      // char* s = js_is_null_or_undefined(pr->property.predicate) ? (char*)0 : js_tosource(ctx, pr->property.predicate);
      dbuf_printf(dbuf, "'%s'", arg);
      if(!js_is_null_or_undefined(pr->property.predicate)) {
        char* src = js_tostring(ctx, pr->property.predicate);
        dbuf_printf(dbuf, ", %s", src);
        js_free(ctx, src);
      }
      JS_FreeCString(ctx, arg);
      break;
    }

    case PREDICATE_MEMBER: {
      js_value_dump(ctx, pr->member.object, dbuf);
      break;
    }

    case PREDICATE_SHIFT: {
      dbuf_printf(dbuf, ">> %d", pr->shift.n);
      dbuf_putc(dbuf, ' ');
      js_value_dump(ctx, pr->shift.predicate, dbuf);
      break;
    }

    case PREDICATE_FUNCTION: {
      int nargs = js_get_propertystr_int32(ctx, pr->function.func, "length");
      dbuf_printf(dbuf, "func(%d)", nargs);
      break;
    }

    default: assert(0); break;
  }
  dbuf_putstr(dbuf, ")");
}

char*
predicate_tostring(const Predicate* pr, JSContext* ctx) {
  DynBuf dbuf;
  js_dbuf_init(ctx, &dbuf);
  predicate_dump(pr, ctx, &dbuf);
  dbuf_0(&dbuf);
  return (char*)dbuf.buf;
}

void
predicate_tosource(const Predicate* pr, JSContext* ctx, DynBuf* dbuf, Arguments* args) {
  Arguments tmp = {0, 0, 0, 0};
  DynBuf abuf;
  js_dbuf_init(ctx, &abuf);

  /*if(args == 0 || args->c <= 0) {
    int i;
    char c[2] = {0, 0};
    tmp.c = predicate_recursive_num_args(pr) + 1;
    tmp.v = js_mallocz(ctx, sizeof(const char*) * (tmp.c + 1));

    for(i = 0; i < tmp.c; i++) {
      c[0] = 'a' + i;
      tmp.v[i] = js_strndup(ctx, c, 1);
    }

    args = &tmp;
  }*/
  if(args == 0) {
    if(!arguments_alloc(&tmp, ctx, predicate_recursive_num_args(pr) + 1)) {
      JS_ThrowOutOfMemory(ctx);
      return;
    }
    args = &tmp;
  }

  switch(pr->id) {
    case PREDICATE_TYPE: {
      const char* arg = arguments_push(args, ctx, "value");
      dbuf_printf(dbuf, "typeof %s == %s", arg, js_value_type_name(pr->type.flags));
      break;
    }

    case PREDICATE_CHARSET: {
      const char* arg = arguments_push(args, ctx, "chars");
      dbuf_printf(dbuf, "'%s'.indexOf(%s) != -1", pr->charset.set, arg);
      break;
    }

    case PREDICATE_STRING: {
      const char* arg = arguments_push(args, ctx, "string");
      dbuf_printf(dbuf, "%s == '", arg);
      dbuf_put_escaped(dbuf, pr->string.str, pr->string.len);
      dbuf_putc(dbuf, '\'');
      break;
    }

    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS: {
      const char* arg = arguments_push(args, ctx, pr->id == PREDICATE_EQUAL ? "value" : "object");

      if(pr->id == PREDICATE_EQUAL)
        dbuf_printf(dbuf, "%s == ", arg);
      else if(pr->id == PREDICATE_INSTANCEOF)
        dbuf_printf(dbuf, "%s instanceof ", arg);
      else if(pr->id == PREDICATE_PROTOTYPEIS)
        dbuf_printf(dbuf, "Object.getPrototypeOf(%s) == ", arg);

      predicate_inspect(pr->unary.predicate, ctx, dbuf, args, 0);
      break;
    }

    case PREDICATE_NOTNOT: {
      const char* arg = arguments_push(args, ctx, "value");
      dbuf_printf(dbuf, "!!%s", arg);
      break;
    }

    case PREDICATE_NOT: {
      const char* arg = arguments_push(args, ctx, "value");
      dbuf_printf(dbuf, "!%s", arg);
      break;
    }

    case PREDICATE_BNOT: {
      const char* arg = arguments_push(args, ctx, "value");
      dbuf_printf(dbuf, "~%s", arg);
      break;
    }

    case PREDICATE_SQRT: {
      const char* arg = arguments_push(args, ctx, "value");
      dbuf_printf(dbuf, "Math.sqrt(%s)", arg);
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {

      JSPrecedence prec = predicate_precedence(pr);
      BOOL parens[2] = {!JS_IsNumber(pr->binary.left), !JS_IsNumber(pr->binary.right)};

      Predicate* other;

      if((other = js_predicate_data(pr->binary.left)))
        if(prec <= predicate_precedence(other))
          parens[0] = FALSE;

      if((other = js_predicate_data(pr->binary.right)))
        if(prec <= predicate_precedence(other))
          parens[1] = FALSE;

      predicate_inspect(pr->binary.left, ctx, dbuf, args, parens[0]);

      dbuf_putstr(dbuf,
                  ((const char* const[]){" + ", " - ", " * ", " / ", " % ", " | ", " & ", " ** ", " atan2 "})[pr->id - PREDICATE_ADD]);

      predicate_inspect(pr->binary.right, ctx, dbuf, args, parens[1]);
      break;
    }

    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      size_t i;
      JSPrecedence prec = predicate_precedence(pr);

      for(i = 0; i < pr->boolean.npredicates; i++) {
        BOOL parens = !JS_IsNumber(pr->boolean.predicates[i]);

        Predicate* other;

        if((other = js_predicate_data(pr->boolean.predicates[i])))
          if(prec <= predicate_precedence(other))
            parens = FALSE;

        if(i > 0)
          dbuf_putstr(dbuf, ((const char* const[]){" || ", " && ", " ^ "})[pr->id - PREDICATE_OR]);

        /*        if(!other)
                  js_value_dump(ctx, pr->boolean.predicates[i], dbuf);
                else */
        predicate_inspect(pr->boolean.predicates[i], ctx, dbuf, args, parens);
      }
      break;
    }

    case PREDICATE_PROPERTY: {
      const char* arg = arguments_push(args, ctx, "object");
      const char* prop = JS_AtomToCString(ctx, pr->property.atom);
      char *x, *s = js_is_null_or_undefined(pr->property.predicate) ? (char*)0 : js_tosource(ctx, pr->property.predicate);
      dbuf_printf(dbuf, "%s.%s", arg, prop);
      if(s) {
        int i, arglen, slen = strlen(s);
        x = s;
        if((arglen = byte_chrs(s, slen, " =", 2)) < slen) {
          for(i = arglen; s[i]; i++)
            if(!is_whitespace_char(s[i]) && s[i] != '=' && s[i] != '>')
              break;
          if(!strncmp(&s[i], s, arglen) && !(is_alphanumeric_char(s[i + arglen]) || is_digit_char(s[i + arglen]))) {
            s += i + arglen;
          }
        } else

            if(!strncmp(s + 1, " => ", 4) && is_alphanumeric_char(s[5]) && is_whitespace_char(s[6])) {
          s += 6;
        }

        dbuf_putstr(dbuf, s);
        js_free(ctx, x);
      }
      JS_FreeCString(ctx, prop);
      break;
    }

    case PREDICATE_MEMBER: {
      break;
    }
      /*
          case PREDICATE_REGEXP: {
            break;
          }
          case PREDICATE_PROPERTY: {
            break;
          }
          case PREDICATE_SHIFT: {
            break;
          }*/

    case PREDICATE_FUNCTION: {
      const char* str = js_function_tostring(ctx, pr->function.func);
      dbuf_putstr(dbuf, str);
      JS_FreeCString(ctx, str);
      break;
    }

    default: {
      assert(0);
      break;
    }
  }

  arguments_dump(args, &abuf);

  dbuf_putstr(&abuf, " => ");
  dbuf_put(&abuf, dbuf->buf, dbuf->size);
  dbuf_0(&abuf);
  dbuf_free(dbuf);
  *dbuf = abuf;
}

JSValue
predicate_regexp_capture(uint8_t** capture, int capture_count, uint8_t* input, JSContext* ctx) {
  int i;
  uint32_t buf[capture_count * 2];
  memset(buf, 0, sizeof(buf));

  for(i = 0; i < 2 * capture_count; i += 2) {
    if(capture[i]) {
      buf[i] = capture[i] - input;
      buf[i + 1] = capture[i + 1] - input;
    }
  }

  return JS_NewArrayBufferCopy(ctx, (const uint8_t*)buf, (capture_count * 2) * sizeof(uint32_t));
}

void
predicate_free_rt(Predicate* pr, JSRuntime* rt) {
  switch(pr->id) {
    case PREDICATE_TYPE: {
      break;
    }

    case PREDICATE_CHARSET: {
      js_free_rt(rt, pr->charset.set);
      vector_free(&pr->charset.chars);
      break;
    }

    case PREDICATE_STRING: {
      js_free_rt(rt, pr->string.str);
      break;
    }

    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      JS_FreeValueRT(rt, pr->unary.predicate);
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      JS_FreeValueRT(rt, pr->binary.left);
      JS_FreeValueRT(rt, pr->binary.right);
      break;
    }

    case PREDICATE_AND:
    case PREDICATE_OR:
    case PREDICATE_XOR: {
      js_values_free(rt, pr->boolean.npredicates, pr->boolean.predicates);
      break;
    }

    case PREDICATE_REGEXP: {
      // if(pr->regexp.bytecode) js_free_rt(rt, pr->regexp.bytecode);
      js_free_rt(rt, pr->regexp.expr.source);
      break;
    }
    case PREDICATE_PROPERTY: {
      JS_FreeAtomRT(rt, pr->property.atom);
      JS_FreeValueRT(rt, pr->property.predicate);

      break;
    }
    case PREDICATE_MEMBER: {
      JS_FreeValueRT(rt, pr->member.object);
      break;
    }
    case PREDICATE_SHIFT: {
      JS_FreeValueRT(rt, pr->shift.predicate);
      break;
    }
    case PREDICATE_FUNCTION: {
      JS_FreeValueRT(rt, pr->function.func);
      JS_FreeValueRT(rt, pr->function.this_val);
      break;
    }
  }
  memset(pr, 0, sizeof(Predicate));
}

JSValue
predicate_values(const Predicate* pr, JSContext* ctx) {
  JSValue ret = JS_UNDEFINED;
  switch(pr->id) {
    case PREDICATE_TYPE:
    case PREDICATE_REGEXP: {
      break;
    }
    case PREDICATE_CHARSET:
    case PREDICATE_STRING: {
      ret = JS_NewArray(ctx);
      JS_SetPropertyUint32(ctx, ret, 0, JS_NewStringLen(ctx, pr->string.str, pr->string.len));
      break;
    }

    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      ret = js_values_toarray(ctx, 1, (JSValue*)&pr->unary.predicate);
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      ret = js_values_toarray(ctx, 2, (JSValue*)&pr->binary.left);
      break;
    }

    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      ret = js_values_toarray(ctx, pr->boolean.npredicates, pr->boolean.predicates);
      break;
    }

    case PREDICATE_PROPERTY: {
      JSValue v[2] = {JS_AtomToValue(ctx, pr->property.atom), pr->property.predicate};

      ret = js_values_toarray(ctx, 2, v);
      JS_FreeValue(ctx, v[0]);

      break;
    }

    case PREDICATE_MEMBER: {
      ret = JS_DupValue(ctx, pr->member.object);
      break;
    }

    case PREDICATE_SHIFT: {
      ret = JS_DupValue(ctx, pr->shift.predicate);
      break;
    }
    case PREDICATE_FUNCTION: {
      ret = JS_DupValue(ctx, pr->function.func);
      break;
    }
  }
  return ret;
}

JSValue
predicate_keys(const Predicate* pr, JSContext* ctx) {
  JSValue ret = JS_NewArray(ctx);
  uint32_t i = 0;
  switch(pr->id) {
    case PREDICATE_TYPE:
    case PREDICATE_CHARSET:
    case PREDICATE_STRING:
    case PREDICATE_REGEXP: {
      break;
    }

    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "predicate"));
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "left"));
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "right"));
      break;
    }

    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      uint32_t n = pr->boolean.npredicates;
      for(i = 0; i < n; i++) JS_SetPropertyUint32(ctx, ret, i, JS_NewInt32(ctx, i));
      break;
    }

    case PREDICATE_PROPERTY: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "atom"));
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "predicate"));
      break;
    }

    case PREDICATE_MEMBER: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "object"));
      break;
    }

    case PREDICATE_SHIFT: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "predicate"));
      break;
    }
    case PREDICATE_FUNCTION: {
      JS_SetPropertyUint32(ctx, ret, i++, JS_NewString(ctx, "func"));
      break;
    }
  }
  return ret;
}

Predicate*
predicate_clone(const Predicate* pr, JSContext* ctx) {
  Predicate* ret = js_mallocz(ctx, sizeof(Predicate));

  ret->id = pr->id;

  switch(pr->id) {
    case PREDICATE_TYPE: {
      ret->type.flags = pr->type.flags;
      break;
    }

    case PREDICATE_CHARSET: {
      ret->charset.len = pr->charset.len;
      ret->charset.set = js_strndup(ctx, pr->charset.set, pr->charset.len);
      vector_copy(&ret->charset.chars, &pr->charset.chars);
      break;
    }

    case PREDICATE_STRING: {
      ret->string.len = pr->string.len;
      ret->string.str = js_strndup(ctx, pr->string.str, pr->string.len);
      break;
    }

    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      ret->unary.predicate = JS_DupValue(ctx, pr->unary.predicate);
      break;
    }

    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      ret->binary.left = JS_DupValue(ctx, pr->binary.left);
      ret->binary.right = JS_DupValue(ctx, pr->binary.right);
      break;
    }

    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      ret->boolean.npredicates = pr->boolean.npredicates;
      ret->boolean.predicates = js_values_dup(ctx, pr->boolean.npredicates, pr->boolean.predicates);
      break;
    }

    case PREDICATE_REGEXP: {
      ret->regexp.expr.source = js_strndup(ctx, pr->regexp.expr.source, pr->regexp.expr.len);
      ret->regexp.expr.len = pr->regexp.expr.len;
      ret->regexp.expr.flags = pr->regexp.expr.flags;
      ret->regexp.bytecode = 0;
      break;
    }
    case PREDICATE_PROPERTY: {
      ret->property.atom = JS_DupAtom(ctx, pr->property.atom);
      ret->property.predicate = JS_DupValue(ctx, pr->property.predicate);
      break;
    }
    case PREDICATE_MEMBER: {
      ret->member.object = JS_DupValue(ctx, pr->member.object);
      break;
    }
    case PREDICATE_SHIFT: {
      ret->shift.n = pr->shift.n;
      ret->shift.predicate = JS_DupValue(ctx, pr->shift.predicate);
      break;
    }
    case PREDICATE_FUNCTION: {
      ret->function.func = JS_DupValue(ctx, pr->function.func);
      ret->function.this_val = JS_DupValue(ctx, pr->function.this_val);
      ret->function.arity = pr->function.arity;
      break;
    }
  }

  return ret;
}

int
predicate_regexp_compile(Predicate* pr, JSContext* ctx) {
  assert(pr->id == PREDICATE_REGEXP);
  assert(pr->regexp.bytecode == 0);

  if((pr->regexp.bytecode = regexp_compile(pr->regexp.expr, ctx)))
    return lre_get_capture_count(pr->regexp.bytecode);

  return 0;
}

int
predicate_recursive_num_args(const Predicate* pr) {
  Predicate* other;
  int n = 0;

  switch(pr->id) {
    case PREDICATE_TYPE:
    case PREDICATE_CHARSET:
    case PREDICATE_STRING:
    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      if(js_is_null_or_undefined(pr->unary.predicate))
        n++;
      else if((other = js_predicate_data(pr->unary.predicate)))
        n += predicate_recursive_num_args(other);
      break;
    }
    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      if(js_is_null_or_undefined(pr->binary.left))
        n++;
      else if((other = js_predicate_data(pr->binary.left)))
        n += predicate_recursive_num_args(other);

      if(js_is_null_or_undefined(pr->binary.right))
        n++;
      else if((other = js_predicate_data(pr->binary.right)))
        n += predicate_recursive_num_args(other);

      break;
    }
    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      int i;
      for(i = 0; i < pr->boolean.npredicates; i++)
        if((other = js_predicate_data(pr->boolean.predicates[i])))
          n += predicate_recursive_num_args(other);
      break;
    }
    case PREDICATE_REGEXP: {
      n += 1;
      break;
    }
    case PREDICATE_PROPERTY: {
      if(pr->property.atom == 0)
        n++;

      if(js_is_null_or_undefined(pr->property.predicate))
        n++;
      else if((other = js_predicate_data(pr->property.predicate)))
        n += predicate_recursive_num_args(other);

      break;
    }
    case PREDICATE_MEMBER: {
      n++;
      break;
    }
    case PREDICATE_SHIFT: {
      n++;
      break;
    }
    case PREDICATE_FUNCTION: {
      n += pr->function.arity;
      break;
    }
  }
  return n;
}

int
predicate_direct_num_args(const Predicate* pr) {
  switch(pr->id) {
    case PREDICATE_TYPE:
    case PREDICATE_CHARSET:
    case PREDICATE_STRING:
    case PREDICATE_EQUAL:
    case PREDICATE_INSTANCEOF:
    case PREDICATE_PROTOTYPEIS:
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: {
      return 1;
    }
    case PREDICATE_ADD:
    case PREDICATE_SUB:
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_BOR:
    case PREDICATE_BAND:
    case PREDICATE_POW:
    case PREDICATE_ATAN2: {
      int n = 0;
      if(js_is_null_or_undefined(pr->binary.left))
        n++;

      if(js_is_null_or_undefined(pr->binary.right))
        n++;

      return n;
    }
    case PREDICATE_OR:
    case PREDICATE_AND:
    case PREDICATE_XOR: {
      return 0;
    }
    case PREDICATE_REGEXP: {
      return 1;
    }
    case PREDICATE_PROPERTY: {
      int n = 0;
      if(pr->property.atom == 0)
        n++;

      if(!js_is_null_or_undefined(pr->property.predicate))
        n++;

      return n;
    }
    case PREDICATE_MEMBER: {
      return 1;
    }
    case PREDICATE_SHIFT: {
      return 1;
    }
    case PREDICATE_FUNCTION: {
      return pr->function.arity;
    }
  }

  return -1;
}

JSPrecedence
predicate_precedence(const Predicate* pr) {
  JSPrecedence ret = -1;
  switch(pr->id) {
    case PREDICATE_TYPE: break;
    case PREDICATE_CHARSET: break;
    case PREDICATE_PROTOTYPEIS: break;
    case PREDICATE_REGEXP: break;
    case PREDICATE_SHIFT: break;

    case PREDICATE_STRING: ret = PRECEDENCE_EQUALITY; break;
    case PREDICATE_EQUAL: ret = PRECEDENCE_EQUALITY; break;
    case PREDICATE_INSTANCEOF: ret = PRECEDENCE_LESS_GREATER_IN; break;
    case PREDICATE_NOTNOT:
    case PREDICATE_NOT:
    case PREDICATE_BNOT:
    case PREDICATE_SQRT: ret = PRECEDENCE_UNARY; break;
    case PREDICATE_ADD:
    case PREDICATE_SUB: ret = PRECEDENCE_ADDITIVE; break;
    case PREDICATE_MUL:
    case PREDICATE_DIV:
    case PREDICATE_MOD:
    case PREDICATE_ATAN2: ret = PRECEDENCE_MULTIPLICATIVE; break;
    case PREDICATE_POW: ret = PRECEDENCE_EXPONENTIATION; break;
    case PREDICATE_BOR: ret = PRECEDENCE_BITWISE_OR; break;
    case PREDICATE_BAND: ret = PRECEDENCE_BITWISE_AND; break;
    case PREDICATE_OR: ret = PRECEDENCE_LOGICAL_OR; break;
    case PREDICATE_AND: ret = PRECEDENCE_LOGICAL_AND; break;
    case PREDICATE_XOR: ret = PRECEDENCE_BITWISE_XOR; break;
    case PREDICATE_PROPERTY: ret = PRECEDENCE_MEMBER_ACCESS; break;
    case PREDICATE_MEMBER:
    case PREDICATE_FUNCTION: ret = PRECEDENCE_MEMBER_ACCESS; break;
  }
  assert(ret != -1);
  return ret;
}

/**
 * @}
 */
