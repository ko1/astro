module {whatever};
import "a" as foo;
import "d" as d {search: "./"};
import "d" as d2 {search: "./"};
import "e" as e {search: "./../lib/jq"};
import "f" as f {search: "./../lib/jq"};
import "data" as $d;

def a: 0;
def c: foo::a + d::c + e::me + d2::h + f::bah;
