# Etc — the passwd/group databases plus a few POSIX system-configuration calls.
#
# The database side reads /etc/passwd and /etc/group directly: koruby links no
# NSS, so getpwnam(3) would see exactly these files anyway.  uname/sysconf/
# confstr/getlogin come from C primitives (builtins/process.c).
module Etc
  Passwd = Struct.new(:name, :passwd, :uid, :gid, :gecos, :dir, :shell,
                      :change, :quota, :age, :class, :comment, :expire)
  Group  = Struct.new(:name, :passwd, :gid, :mem)

  __etc_conf_table.each { |name, value| const_set(name, value) }

  class << self
    def uname   = __etc_uname
    def sysconf(v) = __etc_sysconf(v)
    def confstr(v) = __etc_confstr(v)
    def nprocessors = __etc_sysconf(Etc::SC_NPROCESSORS_ONLN)
    def getlogin = __etc_getlogin || ENV['USER']
    def systmpdir = '/tmp'    # CRuby ignores TMPDIR here
    def sysconfdir = '/etc'

    def passwd(&blk)
      if blk
        __passwd_all.each(&blk)
        nil
      else
        e = (@pw_index ||= 0)
        @pw_index += 1
        __passwd_all[e]
      end
    end

    def getpwent = passwd
    def setpwent = (@pw_index = 0; nil)
    def endpwent = (@pw_index = 0; nil)

    def getpwnam(name)
      raise TypeError, "no implicit conversion of #{name.class} into String" unless name.is_a?(String)
      __passwd_all.find { |p| p.name == name } or
        raise ArgumentError, "can't find user for #{name}"
    end

    def getpwuid(*args)                 # explicit nil is a TypeError; no argument is not
      uid = args.empty? ? Process.uid : args[0]
      raise TypeError, "no implicit conversion of #{uid.class} into Integer" unless uid.is_a?(Integer)
      __passwd_all.find { |p| p.uid == uid } or
        raise ArgumentError, "can't find user for #{uid}"
    end

    def group(&blk)
      if blk
        raise RuntimeError, "parallel iteration" if @gr_iterating
        @gr_iterating = true
        begin
          __group_all.each(&blk)
        ensure
          @gr_iterating = false
        end
        nil
      else
        e = (@gr_index ||= 0)
        @gr_index += 1
        __group_all[e]
      end
    end

    def getgrent = group
    def setgrent = (@gr_index = 0; nil)
    def endgrent = (@gr_index = 0; nil)

    def getgrnam(name)
      raise TypeError, "no implicit conversion of #{name.class} into String" unless name.is_a?(String)
      __group_all.find { |g| g.name == name } or
        raise ArgumentError, "can't find group for #{name}"
    end

    def getgrgid(*args)
      gid = args.empty? ? Process.gid : args[0]
      raise TypeError, "no implicit conversion of #{gid.class} into Integer" unless gid.is_a?(Integer)
      __group_all.find { |g| g.gid == gid } or
        raise ArgumentError, "can't find group for #{gid}"
    end

    private

    # Re-read every call: the caller may have just written the file, and these
    # are not hot paths.
    def __passwd_all
      __read_lines('/etc/passwd').filter_map do |f|
        next if f.size < 7
        Passwd.new(f[0], f[1], f[2].to_i, f[3].to_i, f[4], f[5], f[6])
      end
    end

    def __group_all
      __read_lines('/etc/group').filter_map do |f|
        next if f.size < 4
        Group.new(f[0], f[1], f[2].to_i, f[3].split(',').reject(&:empty?))
      end
    end

    def __read_lines(path)
      return [] unless File.readable?(path)
      File.readlines(path).filter_map do |line|
        line = line.chomp
        next if line.empty? || line.start_with?('#')
        line.split(':', -1)
      end
    end
  end
end
