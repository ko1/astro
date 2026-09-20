# shareable_constant_value: literal
# マジックコメントは (Ractor が無いので) 実質 no-op だが、定数代入は走る。
class Zshc
  TBL = { "a" => 1 }
  LIST = [1, 2]
end
p Zshc::TBL, Zshc::LIST
