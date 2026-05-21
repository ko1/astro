# nil / false / true singletons

p nil
p true
p false

# ==
p nil == nil
p nil == false
p nil == true
p true == true
p true == false
p false == false

# !=
p nil != false
p nil != nil

# in if
if nil   then p "nilT" else p "nilF" end
if true  then p "trueT" else p "trueF" end
if false then p "falseT" else p "falseF" end

# 0 is truthy (Ruby semantics)
if 0     then p "0T" else p "0F" end
if 1     then p "1T" else p "1F" end
if -1    then p "neg1T" else p "neg1F" end

# "" and [] are truthy
if ""    then p "emptyStrT" else p "emptyStrF" end
if []    then p "emptyAryT" else p "emptyAryF" end
