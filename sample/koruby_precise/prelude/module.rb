# Module/Class introspection layered on the C core (ancestors/instance_method/
# attached_object).
class Module
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
      __autoload_table.delete(name)                 # one shot: a failed require must not loop
      ancestors.each { |m| m.__autoload_table.delete(name) if m.respond_to?(:__autoload_table, true) }
      require path
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

  def autoload(name, path)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    unless path.is_a?(String) || path.respond_to?(:to_path) || path.respond_to?(:to_str)
      raise TypeError, "no implicit conversion of #{path.class} into String"
    end
    path = path.respond_to?(:to_path) ? path.to_path : (path.is_a?(String) ? path : path.to_str)
    raise ArgumentError, "empty feature name" if path.empty?
    __autoload_table[n] = path
    nil
  end

  def autoload?(name, inherit = true)
    n = name.is_a?(Symbol) ? name : name.to_str.to_sym
    return nil if const_defined?(n, false)
    if __autoload_table.key?(n)
      p0 = __autoload_table[n]
      return __autoload_loaded?(p0) ? nil : p0     # 既に require 済みなら nil (CRuby)
    end
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
  # already threads a trailing kwargs Hash through splat, so accept + no-op.
  def ruby2_keywords(*names); nil; end

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
end
