program SetTest;
var
  vowels, consonants, all, common: set of integer;
  i: integer;

begin
  vowels     := [1, 5, 9, 15, 21];   { stand-in element values }
  consonants := [2..4, 6..8, 10..14, 16..20, 22..25];
  all        := vowels + consonants;
  common     := vowels * [1, 5, 30];
  writeln('5 in vowels = ', 5 in vowels);
  writeln('5 in consonants = ', 5 in consonants);
  writeln('all has 21 = ', 21 in all);
  writeln('common has 5 = ', 5 in common);
  writeln('common has 9 = ', 9 in common);
  for i := 0 to 9 do
    if i in vowels then write('V') else write('-');
  writeln
end.
