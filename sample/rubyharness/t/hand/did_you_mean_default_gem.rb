# did_you_mean is a default gem CRuby loads at startup: error objects gain
# #corrections / #spell_checker and the "Did you mean?" line.  dry-container gates
# on `defined?(DidYouMean::KeyErrorChecker)` and dry-core's specs assert the
# KeyError carries a spell checker.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

t(:loaded)    { [defined?(DidYouMean), defined?(DidYouMean::KeyErrorChecker), DidYouMean::VERSION] }
t(:spell)     { DidYouMean::SpellChecker.new(dictionary: %w[foo bar baz]).correct("fooo") }
t(:jaro)      { [DidYouMean::Jaro.distance("foo", "fooo").round(4), DidYouMean::JaroWinkler.distance("foo", "fooo").round(4)] }
t(:lev)       { DidYouMean::Levenshtein.distance("foo", "fooo") }
t(:key_err)   { {foo: 1, bar: 2}.fetch(:fooo) rescue [$!.class, $!.spell_checker.class, $!.corrections] }
t(:key_msg)   { {foo: 1}.fetch(:fooo) rescue $!.detailed_message(highlight: false) }
t(:name_err)  { Objject rescue [$!.class, $!.corrections, $!.original_message] }
t(:nme)       { "x".start_wtih?("a") rescue [$!.class, $!.corrections] }
t(:ivar)      { @full_name = 1; @full_anme.nil? }
t(:correct)   { DidYouMean.correct_error(ArgumentError, DidYouMean::KeyErrorChecker); DidYouMean.spell_checkers["ArgumentError"] }
t(:tree)      { DidYouMean::TreeSpellChecker.new(dictionary: ["spec/models/user_spec.rb"]).correct("spec/model/user_spec.rb") }
t(:checkers)  { %w[NameError KeyError NoMethodError ZeroDivisionError].map { |k| c = DidYouMean.spell_checkers[k]; c.is_a?(Class) ? c.name : c.class.name } }
t(:correctable) { [KeyError.ancestors.include?(DidYouMean::Correctable), NameError.ancestors.include?(DidYouMean::Correctable)] }

# a struct/keyword-ish miss goes through the same checker
Point = Struct.new(:x, :y)
t(:struct)    { Point.new(1, 2).to_h.fetch(:z) rescue [$!.class, $!.corrections] }
t(:no_corr)   { {a: 1}.fetch(:zzzzzzzz) rescue [$!.class, $!.corrections] }
