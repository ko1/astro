# Module/Class introspection layered on the C core (ancestors/instance_method/
# attached_object).
class Module
  # The default #const_added hook: nil.  Defining it makes the name show up in
  # Module.private_instance_methods; the C side only dispatches when a module
  # overrides it (korb_mod_hook_custom).
  def const_added(name)
    nil
  end
  private :const_added

  # The modules in the ancestor chain that are not classes (self excluded).
  def included_modules
    ancestors.select { |m| !m.is_a?(Class) && !m.equal?(self) }   # a Module subclass's instance counts
  end

  # Like instance_method, but a non-public method is a NameError.
  def public_instance_method(name)
    m = ::Module.instance_method(:instance_method).bind_call(self, name)   # not a singleton #instance_method override (CRuby)
    vis = if private_method_defined?(name) then "private"
          elsif protected_method_defined?(name) then "protected"
          end
    if vis
      kind = instance_of?(Module) ? "module" : "class"
      raise NameError.new("method '#{name}' for #{kind} '#{self}' is #{vis}", name.to_sym)
    end
    m
  end

  # Ruby 4.1: every live module/class whose ancestors include self, minus self
  # and the singleton classes.  A later `include` has to show up here, so this
  # is a live scan rather than a list maintained at include time.
  def descendants
    out = []
    ObjectSpace.each_object(Module) do |m|
      next if m.equal?(self)
      next if m.is_a?(Class) && m.singleton_class?
      out << m if m.ancestors.include?(self)
    end
    out
  end

  # True iff self is a singleton class.  attached_object succeeds only for a
  # singleton class (raises TypeError otherwise), so use it as the predicate.
  # It is a Class method, so a plain Module answers false outright (calling it
  # there would be a NameError, not a TypeError).
  def singleton_class?
    return false unless is_a?(Class)
    attached_object
    true
  rescue TypeError
    false
  end

  # Refinements defined directly in this module (docs/refinements.md).  The C
  # side keeps them as a flat [target, refinement, ...] Array.
  def refinements
    flat = instance_variable_get(:@__refinements)
    return [] unless flat
    out = []
    i = 1
    while i < flat.size
      out << flat[i]
      i += 2
    end
    out
  end
  # (Module#undefined_instance_methods is the C one; it used to be stubbed here
  # and only worked because Class carried its own copy of the C entry.)

  # Default hooks (private).  const_added is a no-op; const_missing raises.
  # (The automatic firing of these hooks is a separate C concern.)
  def const_missing(name)
    # An `autoload :Const, "path"` registration gets its chance here: require the
    # file (once) and retry the constant.  Ancestors are searched too, matching
    # CRuby (an autoload on a superclass answers a subclass's lookup).
    fired = __autoload_fire(name)
    if fired
      owner, path = fired
      # koruby の const 表は top-level 定数を Object 所有として持たないので、
      # const_defined? ではなく実際に引いてみる (テーブルからは既に外して
      # あるので、まだ無ければ通常の NameError に落ちる)。
      begin
        return const_get(name)
      rescue NameError
      end
      # The file may have defined it in the lexical scope that registered the
      # autoload, not here (CRuby then leaves this scope without the constant).
      if owner && !owner.equal?(self)
        begin
          return owner.const_get(name)
        rescue NameError
        end
      end
      # ...or in an enclosing lexical scope.  CRuby then drops the registration
      # (already done above), warns in verbose mode, and answers from the parent.
      scope = __lexical_parent
      while scope
        begin
          v = scope.const_get(name, false)   # __autoload_fire already warned in verbose mode
          return v
        rescue NameError
        end
        scope = scope.send(:__lexical_parent)
      end
    end
    # CRuby names the namespace with #name when it has one, else #inspect
    # (both may be user-defined, and #to_s is NOT consulted).
    label = begin
      n = self.name
      n.nil? ? inspect : n
    rescue StandardError
      inspect
    end
    msg = self.equal?(Object) ? "uninitialized constant #{name}" : "uninitialized constant #{label}::#{name}"
    # CRuby's default const_missing pops its own frame (rb_mod_const_missing), so
    # the NameError is reported at the REFERENCE, not inside const_missing.
    e = NameError.new(msg, name, receiver: self)   # NameError#name / #receiver reflect the missing constant
    e.set_backtrace(caller_locations)   # Locations, so #backtrace_locations answers too (error_highlight)
    raise e
  end

  # CRuby fires a pending autoload at LOOKUP time, not through const_missing.  A
  # module with its own const_missing (dry-types) must still get its autoloads,
  # so the constant-read path calls this hook directly and retries the lookup.
  # Returns [owner module, path], or nil when there was no registration.
  def __autoload_fire(name)
    owner, path = __autoload_owner_for(name)
    return nil unless path
    # require が失敗したら登録は残す (CRuby と同じ: 次の参照でも LoadError。
    # 先に消すと、同じ定数を待っている別スレッドが登録を見失って NameError
    # になる)。成功したときだけ一度きりの登録を外す。
    busy = owner || self                            # other threads see the constant only once this is done
    busy.send(:__autoload_busy, name, true)
    begin
      Module.__autoload_require(path)               # main.require (CRuby; mockable in specs)
    ensure
      busy.send(:__autoload_busy, name, false)
    end
    __autoload_drop(name)
    owner.send(:__autoload_drop, name) if owner
    ancestors.each { |m| m.send(:__autoload_drop, name) if m.respond_to?(:__autoload_drop, true) }
    __autoload_warn_parent(name, path) if $VERBOSE
    [owner, path]
  end
  private :__autoload_fire

  # CRuby warns (verbose only) when the required file defined the constant in an
  # ENCLOSING scope instead of the one that registered the autoload.
  private def __autoload_warn_parent(name, path)
    return if const_defined?(name, false)
    scope = __lexical_parent
    while scope
      if scope.const_defined?(name, false)
        warn "warning: Expected #{path} to define #{self}::#{name} but it didn't"
        return
      end
      scope = scope.send(:__lexical_parent)
    end
  end

  # ---- autoload -------------------------------------------------------------
  # koruby resolves constants eagerly, so `autoload` is recorded per module and
  # honoured from #const_missing (rather than at constant-table lookup time).
  def __autoload_table
    @__autoloads ||= {}
  end
  private :__autoload_table

  # Read-only view: `autoload?` / the resolver must not create the table, since
  # a FROZEN module (the singleton class of a frozen object, say) can still be
  # asked — CRuby's autoload? never writes.
  def __autoload_table_r
    @__autoloads || {}
  end
  private :__autoload_table_r

  # Drop one registration WITHOUT creating the table: a frozen module can still
  # be an ancestor of the one whose autoload just fired.
  def __autoload_drop(name)
    t = instance_variable_defined?(:@__autoloads) ? @__autoloads : nil
    t.delete(name) if t
  end
  private :__autoload_drop

  # CRuby loads an autoload file through main's #require, so a spec can mock it.
  def self.__autoload_require(path)
    m = TOPLEVEL_BINDING.eval("self")
    m.__send__(:require, path)     # Kernel#require is private; main's is not a real singleton here
  end

  # A dup'ed module shares the pending autoload registrations (CRuby copies the
  # table; a still-pending entry keeps working through the copy).
  def initialize_copy(other)
    super
    if other.instance_variable_defined?(:@__autoloads)
      @__autoloads = other.instance_variable_get(:@__autoloads).dup
    end
    self
  end

  private def __autoload_path_for(name)
    o, path = __autoload_owner_for(name)
    path
  end

  # Which module's pending autoload answers a bare `name` here: self and its
  # ancestors first, then the lexically enclosing scopes (CRuby resolves a bare
  # constant through Module.nesting before the ancestry).  Returns [owner, path].
  private def __autoload_owner_for(name)
    n = name.to_sym
    scope = self
    while scope
      t = scope.send(:__autoload_table_r)
      return [scope, t[n]] if t.key?(n)
      scope.ancestors.each do |m|
        next if m.equal?(scope)
        next unless m.respond_to?(:__autoload_table_r, true)
        tbl = m.send(:__autoload_table_r)
        return [m, tbl[n]] if tbl.key?(n)
      end
      scope = scope.send(:__lexical_parent)
    end
    [nil, nil]
  end

  # `class M::C` / `module M::C` where C is a pending autoload: the file has to be
  # required first so the body reopens what it defined (creating a fresh class
  # here would silently discard the file's definition).  Unlike #const_missing
  # this stays quiet when the file leaves the constant undefined — the caller
  # then creates the class as usual.  LoadError still propagates, as in CRuby.
  def __autoload_open(name)
    path = __autoload_path_for(name)
    return nil unless path
    Module.__autoload_require(path)
    __autoload_table.delete(name.to_sym)
    nil
  end
  private :__autoload_open

  def autoload(name, path)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    raise NameError, "autoload must be constant name: #{n}" unless n.to_s =~ /\A\p{Upper}\p{Word}*\z/
    unless path.is_a?(String) || path.respond_to?(:to_path) || path.respond_to?(:to_str)
      raise TypeError, "no implicit conversion of #{path.class} into String"
    end
    path = path.respond_to?(:to_path) ? path.to_path : (path.is_a?(String) ? path : path.to_str)
    raise ArgumentError, "empty feature name" if path.empty?
    raise FrozenError.new("can't modify frozen #{self.class}: #{inspect}", receiver: self) if frozen?
    __autoload_table[n] = path
    # CRuby answers #const_source_location with the autoload REGISTRATION site
    # until the constant is really defined.
    if (loc = caller_locations(1, 1)&.first)
      __const_reg_loc(n, loc.path, loc.lineno)
    end
    const_added(n) if respond_to?(:const_added, true)   # an autoload defines the constant (CRuby)
    nil
  end

  def autoload?(name, inherit = true)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    # 自分の登録が先: const_defined? は未ロードの autoload も真を返すので、
    # 先に問うと自分の登録を見失う
    if __autoload_table_r.key?(n)
      p0 = __autoload_table_r[n]
      return __autoload_loaded?(p0) ? nil : p0     # 既に require 済みなら nil (CRuby)
    end
    return nil if const_defined?(n, false)
    return nil unless inherit
    ancestors.each do |m|
      next if m.equal?(self)
      next unless m.respond_to?(:__autoload_table_r, true)
      tbl = m.send(:__autoload_table_r)
      # const_defined? counts a PENDING autoload as defined, so it cannot be used
      # to tell "already loaded" here — __autoload_loaded? below does that.
      next unless tbl.key?(n)
      return __autoload_loaded?(tbl[n]) ? nil : tbl[n]
    end
    nil
  end

  # その feature が既に読み込まれているか。判定は C 側と共通 (末尾一致に加えて
  # 「今まさに読み込み中」を扱う: 読んでいるスレッドからだけ隠す)。
  private def __autoload_loaded?(path)
    __autoload_feature_loaded?(path)
  end
  private def const_added(name); end

  # ruby2_keywords marks methods to pass a bare kwargs hash through *args: the
  # flagged method makes the mark on the collected Hash persistent, and a splat
  # call site turns it back into keywords.
  # private, like CRuby: rspec-mocks gates its proxy on
  # `Module.private_method_defined?(:ruby2_keywords)`.
  private def ruby2_keywords(*names)
    names.each do |n|
      # the lookup and the shape check are C: asking SELF for #method_defined? /
      # #instance_method would be visible to a partial double stubbing them
      case __set_ruby2_keywords(self, n)
      when 0 then raise NameError.new("undefined method '#{n}' for class '#{self}'", n)
      when 3 then raise TypeError, "#{n.inspect} is not a symbol nor a string"
      when 2 then warn "Skipping set of ruby2_keywords flag for #{n} (method accepts keywords " \
                       "or method does not accept argument splat)", uplevel: 1
      end
    end
    nil
  end

  # The primitive behind Object#extend (private): mix self into obj's singleton
  # class.  append_features, not include — CRuby does not fire #included here.
  private def extend_object(obj)
    raise TypeError, "wrong argument type Class (expected Module)" if is_a?(Class)   # rebound to a Class
    k = ::Kernel   # obj may be a BasicObject
    sc = k.instance_method(:singleton_class).bind_call(obj)   # TypeError for Integer / Symbol first (CRuby)
    # nil / true / false extend their own class (NilClass ...), which is not frozen
    if !(nil.equal?(obj) || true.equal?(obj) || false.equal?(obj)) && k.instance_method(:frozen?).bind_call(obj)
      raise FrozenError.new("can't modify frozen #{k.instance_method(:class).bind_call(obj)}: #{obj.inspect}", receiver: obj)
    end
    ::Module.instance_method(:append_features).bind_call(self, sc)   # the primitive: an overridden #append_features is not called (CRuby)
    obj
  end
end

module Kernel
  # 単独の `autoload :X, "path"` / `Kernel.autoload` は呼び出し元の cref（メソッドならその
  # クラス・モジュール、トップレベルは Object）に登録する (CRuby の rb_vm_cbase)。
  private def autoload(name, path) = __korb_caller_cref.autoload(name, path)
  private def autoload?(name, inherit = true) = __korb_caller_cref.autoload?(name, inherit)
  def self.autoload(name, path) = __korb_caller_cref.autoload(name, path)
  def self.autoload?(name, inherit = true) = __korb_caller_cref.autoload?(name, inherit)
end

# main#define_method: Object に public メソッドを定義して Symbol を返す。
# Object の private メソッドにする（CRuby は main の特異クラス）。toplevel def は global
# 関数表に入り Kernel.define_method まで奪って、Kernel に付けたはずのメソッドが Object に
# 載っていた（irb のテストの require 差し替え）。Module 側は Module#define_method が先に当たる。
class Object
  private def define_method(name, method = nil, &block)
    if method
      Object.__send__(:define_method, name, method)
    else
      Object.__send__(:define_method, name, &block)
    end
    Object.__send__(:public, name)
    name.to_sym
  end
end

class Class
  # Reachable only through an explicit `send(:initialize)`: Class.new builds the
  # class in C without going through it, so any call here is a re-initialization.
  def initialize(_superclass = nil)
    raise TypeError, "already initialized class"
  end
  private :initialize
end

# The anonymous module `Module#refine` creates (docs/refinements.md).  Its
# #class is pinned to Refinement by the C side; the refined class is kept in
# @__refine_target.
class Refinement < Module
  # the refined class's methods are visible through it (CRuby: it is the refinement's superclass)
  def instance_methods(all = true) = all ? (super(all) | target.instance_methods) : super(all)
  def target
    instance_variable_get(:@__refine_target)
  end

  # Copy the OWN Ruby-defined methods of each module into this refinement.
  def import_methods(*mods)
    __refinement_import(self, mods)
  end

  # CRuby removed these on Refinement in 3.2 (a refinement cannot gain methods
  # from another module, and it is never itself included/prepended/extended).
  def include(*)
    raise TypeError, "Refinement#include has been removed"
  end

  def prepend(*)
    raise TypeError, "Refinement#prepend has been removed"
  end

  undef_method :append_features, :prepend_features, :extend_object
end

# main's own singleton methods, for reflection (CRuby: main.singleton_methods is
# [:inspect, :to_s]; main.method(:ruby2_keywords).owner is main's singleton class).
# include / public / private / define_method / using stay on the C paths: a
# no-argument `private` has to reach the caller's default visibility.
# main.c calls this once `main` exists (after the prelude).
def Object.__korb_main_setup(main)
  main.singleton_class.class_eval do
    def inspect = "main"
    def to_s = "main"
    private def ruby2_keywords(*names) = ::Object.__send__(:ruby2_keywords, *names)
  end
  nil
end
