require_relative "../../test_helper"

# Pathname — basic ops over the bootstrap implementation.

def test_basic_construction
  p = Pathname.new("/usr/local/bin")
  assert_equal "/usr/local/bin", p.to_s
  assert_equal "/usr/local/bin", p.to_path
end

def test_pathname_helper_function
  p = Pathname("foo/bar")
  assert(p.is_a?(Pathname))
  assert_equal "foo/bar", p.to_s
  # Pathname() returns same object if already Pathname
  q = Pathname(p)
  assert(q.equal?(p))
end

def test_join_via_plus
  p = Pathname.new("/etc")
  q = p + "passwd"
  assert_equal "/etc/passwd", q.to_s
end

def test_join_method
  p = Pathname.new("/a")
  assert_equal "/a/b/c", p.join("b", "c").to_s
end

def test_slash_alias
  p = Pathname.new("/usr") / "bin" / "ruby"
  assert_equal "/usr/bin/ruby", p.to_s
end

def test_basename_dirname_extname
  p = Pathname.new("/foo/bar/baz.txt")
  assert_equal "baz.txt", p.basename.to_s
  assert_equal "/foo/bar", p.dirname.to_s
  assert_equal ".txt", p.extname
end

def test_absolute_relative
  assert Pathname.new("/abs").absolute?
  assert Pathname.new("rel/path").relative?
end

def test_equality
  a = Pathname.new("/x")
  b = Pathname.new("/x")
  c = Pathname.new("/y")
  assert_equal a, b
  assert(a != c)
end

def test_exist_directory_file
  cwd = Pathname.new(Dir.pwd)
  assert cwd.exist?
  assert cwd.directory?
  assert !cwd.file?
end

def test_split
  p = Pathname.new("/a/b/c")
  d, b = p.split
  assert_equal "/a/b", d.to_s
  assert_equal "c",    b.to_s
end

def test_each_filename
  parts = []
  Pathname.new("/foo/bar/baz").each_filename { |x| parts << x }
  assert_equal ["foo", "bar", "baz"], parts
end

def test_parent
  assert_equal "/foo", Pathname.new("/foo/bar").parent.to_s
end

def test_root_p
  assert Pathname.new("/").root?
  assert !Pathname.new("/a").root?
end

# OS ops — exercise mkdir / rmdir / write / read / size / chmod /
# rename / unlink in a fresh temp dir.
def test_os_ops_round_trip
  base = Pathname.new("/tmp/koruby_path_ops_#{Process.pid rescue rand(99999)}")
  base.unlink rescue nil
  base.mkdir
  begin
    a = base.join("a.txt")
    b = base.join("b.txt")
    a.write("hi")
    b.write("longer content")
    assert_equal 2,  a.size
    assert_equal 14, b.size
    children = base.children.map(&:to_s).sort
    assert_equal 2, children.size
    a.chmod(0o644)
    a.rename("#{base}/a2.txt")
    a2 = base.join("a2.txt")
    assert a2.exist?
    assert !a.exist?
    a2.unlink
    b.unlink
  ensure
    base.rmdir rescue nil
  end
end

def test_each_child_block_and_no_block
  base = Pathname.new("/tmp/koruby_each_child_test")
  base.unlink rescue nil
  base.mkdir
  begin
    base.join("x").write("a")
    base.join("y").write("b")
    seen = []
    base.each_child { |c| seen << c.basename.to_s }
    assert_equal ["x", "y"], seen.sort
    arr = base.each_child
    assert_equal 2, arr.size
  ensure
    base.children.each(&:unlink) rescue nil
    base.rmdir rescue nil
  end
end

TESTS = %i[
  test_basic_construction test_pathname_helper_function
  test_join_via_plus test_join_method test_slash_alias
  test_basename_dirname_extname test_absolute_relative
  test_equality test_exist_directory_file
  test_split test_each_filename test_parent test_root_p
  test_os_ops_round_trip test_each_child_block_and_no_block
]
TESTS.each {|t| run_test(t) }
report "Pathname"
