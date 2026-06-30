p [1, 2, 3].pack("w*").bytes
p [128, 256].pack("w*").bytes
p [1, 2, 3].pack("w2").bytes
p "\x01\x82\x00".unpack("w*")
p [127, 128, 16383, 16384].pack("w*").unpack("w*")
