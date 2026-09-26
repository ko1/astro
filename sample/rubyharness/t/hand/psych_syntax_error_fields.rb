# Psych::SyntaxError carries psych's own fields: problem then context in the
# message, the CONTEXT mark's line/column (rubocop matches the message).
require 'yaml'
["AllCops:\n  Exclude:\n    - **/*_old.rb\n", "a: [1, 2\n", "a: b: c\n", "- a\nb: 1\n", "\"x\n"].each do |src|
  begin
    YAML.safe_load(src, filename: "x.yml")
  rescue Psych::SyntaxError => e
    p [e.message, e.line, e.column, e.offset, e.problem, e.context]
  end
end
