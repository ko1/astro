# DidYouMean is autoloaded (CRuby 4.1, Feature #21951): nothing is loaded at boot, and the
# first Exception#detailed_message requires it and re-dispatches to the decorator.
p Object.autoload?(:DidYouMean), defined?(DidYouMean) ? :def : :undef, $LOADED_FEATURES.grep(/did_you_mean\.rb/).size
begin
  [].frist
rescue NoMethodError => e
  p e.respond_to?(:corrections)
  p e.detailed_message(highlight: false).lines.last
  p e.respond_to?(:corrections), e.corrections
end
p Object.autoload?(:DidYouMean), $LOADED_FEATURES.grep(/did_you_mean\.rb/).size
begin; {a: 1}.fetch(:b); rescue KeyError => e; puts e.full_message(highlight: false); end
