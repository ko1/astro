class TI; def to_int; 42; end; end
p Time.at(TI.new).to_i
p (begin; Time.at("x"); rescue TypeError; "TE"; end)
p (begin; Time.at(nil); rescue TypeError; "TE"; end)
p Time.at(Time.at(5)).to_i
p Time.at(0, 3.5).usec
p Time.at(0, 0.5).usec
p Time.at(0, 4).usec
p Time.at(0, 100).usec
