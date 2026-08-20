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

  # The modules in the ancestor chain that are not classes.
  def included_modules
    ancestors.select { |m| m.instance_of?(Module) }
  end

  # koruby does not track method visibility strictly; public_instance_method is
  # instance_method for a public/protected method (a private one should raise,
  # but the distinction isn't modelled here).
  def public_instance_method(name)
    instance_method(name)
  end

  # True iff self is a singleton class.  attached_object succeeds only for a
  # singleton class (raises TypeError otherwise), so use it as the predicate.
  def singleton_class?
    attached_object
    true
  rescue TypeError
    false
  end

  # koruby has no refinements; report none.
  def refinements; []; end
  def undefined_instance_methods; []; end

  # Default hooks (private).  const_added is a no-op; const_missing raises.
  # (The automatic firing of these hooks is a separate C concern.)
  def const_missing(name)
    # An `autoload :Const, "path"` registration gets its chance here: require the
    # file (once) and retry the constant.  Ancestors are searched too, matching
    # CRuby (an autoload on a superclass answers a subclass's lookup).
    if (path = __autoload_path_for(name))
      # require が失敗したら登録は残す (CRuby と同じ: 次の参照でも LoadError。
      # 先に消すと、同じ定数を待っている別スレッドが登録を見失って NameError
      # になる)。成功したときだけ一度きりの登録を外す。
      require path
      __autoload_table.delete(name)
      ancestors.each { |m| m.__autoload_table.delete(name) if m.respond_to?(:__autoload_table, true) }
      # koruby の const 表は top-level 定数を Object 所有として持たないので、
      # const_defined? ではなく実際に引いてみる (テーブルからは既に外して
      # あるので、まだ無ければ通常の NameError に落ちる)。
      begin
        return const_get(name)
      rescue NameError
      end
    end
    msg = self.equal?(Object) ? "uninitialized constant #{name}" : "uninitialized constant #{self}::#{name}"
    raise NameError.new(msg, name, receiver: self)   # NameError#name / #receiver reflect the missing constant
  end

  # ---- autoload -------------------------------------------------------------
  # koruby resolves constants eagerly, so `autoload` is recorded per module and
  # honoured from #const_missing (rather than at constant-table lookup time).
  def __autoload_table
    @__autoloads ||= {}
  end

  private def __autoload_path_for(name)
    n = name.to_sym
    t = __autoload_table
    return t[n] if t.key?(n)
    ancestors.each do |m|
      next if m.equal?(self)
      next unless m.respond_to?(:__autoload_table, true)
      tbl = m.__autoload_table
      return tbl[n] if tbl.key?(n)
    end
    nil
  end

  # `class M::C` / `module M::C` where C is a pending autoload: the file has to be
  # required first so the body reopens what it defined (creating a fresh class
  # here would silently discard the file's definition).  Unlike #const_missing
  # this stays quiet when the file leaves the constant undefined — the caller
  # then creates the class as usual.  LoadError still propagates, as in CRuby.
  def __autoload_open(name)
    path = __autoload_path_for(name)
    return nil unless path
    require path
    __autoload_table.delete(name.to_sym)
    nil
  end

  def autoload(name, path)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    raise NameError, "autoload must be constant name: #{n}" unless n.to_s =~ /\A[A-Z][A-Za-z0-9_]*\z/
    unless path.is_a?(String) || path.respond_to?(:to_path) || path.respond_to?(:to_str)
      raise TypeError, "no implicit conversion of #{path.class} into String"
    end
    path = path.respond_to?(:to_path) ? path.to_path : (path.is_a?(String) ? path : path.to_str)
    raise ArgumentError, "empty feature name" if path.empty?
    __autoload_table[n] = path
    const_added(n) if respond_to?(:const_added, true)   # an autoload defines the constant (CRuby)
    nil
  end

  def autoload?(name, inherit = true)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    # 自分の登録が先: const_defined? は未ロードの autoload も真を返すので、
    # 先に問うと自分の登録を見失う
    if __autoload_table.key?(n)
      p0 = __autoload_table[n]
      return __autoload_loaded?(p0) ? nil : p0     # 既に require 済みなら nil (CRuby)
    end
    return nil if const_defined?(n, false)
    return nil unless inherit
    ancestors.each do |m|
      next if m.equal?(self)
      next unless m.respond_to?(:__autoload_table, true)
      tbl = m.__autoload_table
      next unless tbl.key?(n) && !m.const_defined?(n, false)
      return __autoload_loaded?(tbl[n]) ? nil : tbl[n]
    end
    nil
  end

  # その feature が既に読み込まれているか ($LOADED_FEATURES は絶対パスなので
  # 拡張子の有無を吸収して末尾一致で見る)。
  private def __autoload_loaded?(path)
    stem = path.end_with?(".rb") ? path : path + ".rb"
    $LOADED_FEATURES.any? { |f| f == path || f.end_with?("/" + stem) || f == stem }
  end
  private def const_added(name); end

  # ruby2_keywords marks methods to pass a bare kwargs hash through *args.  koruby
  # tags every keyword Hash at the call site and the tag rides through splat, so
  # the delegation already works; this only validates the names.
  def ruby2_keywords(*names)
    names.each do |n|
      unless method_defined?(n) || private_method_defined?(n) || respond_to?(n, true)
        raise NameError.new("undefined method '#{n}' for class '#{self}'", n)
      end
      # the flag only means anything on a `*rest`-only signature
      params = (instance_method(n).parameters rescue [])
      ok = params.any? { |t, _| t == :rest } &&
           params.none? { |t, _| t == :key || t == :keyreq || t == :keyrest }
      unless ok
        warn "Skipping set of ruby2_keywords flag for #{n} (method accepts keywords " \
             "or method does not accept argument splat)", uplevel: 1
      end
    end
    nil
  end

  # The primitive behind Object#extend (private): mix self into obj's singleton class.
  private def extend_object(obj)
    obj.singleton_class.include(self)
    obj
  end
end

module Kernel
  # 単独の `autoload :X, "path"` は Object への登録 (CRuby と同じ)。
  private def autoload(name, path) = Object.autoload(name, path)
  private def autoload?(name, inherit = true) = Object.autoload?(name, inherit)
  # `Kernel.autoload` も同じく Object に登録する (Kernel 自身ではない)。
  def self.autoload(name, path) = Object.autoload(name, path)
  def self.autoload?(name, inherit = true) = Object.autoload?(name, inherit)
end
