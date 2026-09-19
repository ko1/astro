# Class.new / Module.new の body は CRuby では mod.module_eval(&blk) 相当で、
# 新しいクラスが self かつ default definee になる。転送された Proc
# (`Class.new(Base, &blk)`) でも同じでなければならない — koruby は転送 Proc の
# self をそのまま使うので `def` がどこにも行かず、Module.new(&proc{...}) では
# SEGV していた。minitest は fake なテストクラスをこの形で組み立てる。
class Base; end
def make_forwarded(&blk); Class.new(Base, &blk); end
def make_splat_fwd(*a, &blk); Class.new(*a, &blk); end

c1 = Class.new(Base) { def hello; :literal; end }
p c1.instance_methods(false), c1.new.hello

c2 = make_forwarded { def hello; :forwarded; end }
p c2.instance_methods(false), c2.new.hello

c3 = make_splat_fwd(Base) { def hello; :splat; end }
p c3.instance_methods(false), c3.new.hello

pr = proc { def hello; :proc_var; end }
c4 = Class.new(Base, &pr)
p c4.instance_methods(false), c4.new.hello

m = Module.new(&proc { def mm; :m; end })
p m.instance_methods(false)

# body の self は新しいクラス
seen = nil
c5 = Class.new(Base, &proc { seen = self })
p seen.equal?(c5)

# ブロック引数にも新しいクラスが渡る
got = nil
c6 = Class.new(Base, &proc { |k| got = k })
p got.equal?(c6)

# superclass は効いている
p c2.superclass, c2.ancestors.include?(Base)
