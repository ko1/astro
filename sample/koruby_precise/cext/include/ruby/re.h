/* koruby: the Regexp API that goes through Ruby (Regexp.new, MatchData#[]); no Onigmo structures */
#include "../ruby.h"
#ifndef KORUBY_RE_H
#define KORUBY_RE_H
#define ONIG_OPTION_NONE       0U
#define ONIG_OPTION_IGNORECASE 1U
#define ONIG_OPTION_EXTEND     2U
#define ONIG_OPTION_MULTILINE  4U
VALUE rb_reg_new(const char *src, long len, int options);
VALUE rb_reg_new_str(VALUE src, int options);
VALUE rb_reg_regcomp(VALUE str);
VALUE rb_reg_match(VALUE re, VALUE str);
VALUE rb_reg_nth_match(int nth, VALUE match);
VALUE rb_reg_last_match(VALUE match);
VALUE rb_backref_get(void);
void  rb_backref_set(VALUE match);
void  rb_match_busy(VALUE match);
#endif
