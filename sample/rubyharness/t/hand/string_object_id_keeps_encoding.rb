# #object_id / __id__ on a String keeps its encoding, and stays stable.
s = "abc"; i = s.object_id; p s.encoding, s.object_id == i, s.__id__ == i
a = "abc".force_encoding("US-ASCII"); a.__id__; p a.encoding
b = "é".b; b.object_id; p b.encoding, b.bytes
e = "x".encode("EUC-JP"); e.object_id; p e.encoding
GC.start; p s.object_id == i, [1].object_id.is_a?(Integer)
