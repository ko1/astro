# Ractor#receive / #recv are private and read the current ractor (receive inside Ractor.new { })
$VERBOSE = nil
r = Ractor.new { receive * 2 }; r.send(21); p r.value
r = Ractor.new { recv + 1 }; r << 1; p r.value
p Ractor.private_instance_methods(false).sort
