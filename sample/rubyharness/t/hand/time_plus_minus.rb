t = Time.at(100)
p (t + 50).to_i
p (t - 30).to_i
p (t - Time.at(40))
p (begin; t + "5"; rescue TypeError; "TE"; end)
p (begin; t - "5"; rescue TypeError; "TE"; end)
p (begin; t + t; rescue TypeError; "TE"; end)
