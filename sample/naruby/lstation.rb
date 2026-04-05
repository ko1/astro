#!/usr/bin/env ruby
# astrojitd.rb
#
# Ruby server for ASTro JIT: L1 and L2 (single script, selectable mode).
#
# Protocol (fixed 16-byte header):
#   header = [type:uint32_be, size:uint32_be, hash:uint64_be]
#   message = header + payload(size bytes)
#
# Message types:
#   QUERY(h)           : payload empty
#   COMPILE(h, src)    : payload = C source (bytes)
#   HIT(h)             : payload empty            (L1->L0 only)
#   MISS(h)            : payload empty
#   OBJ(h, obj_bytes)  : payload = raw .o bytes   (L2->L1, and internal)
#   ERR(h, msg)        : payload = error string (bytes)
#
# Important semantic:
#   L2 QUERY-hit replies with OBJ (not HIT). This allows L1 to materialize .so
#   and then reply HIT to L0.
#
# Topology:
#   L0 <-> L1 : Unix domain socket (stream)
#   L1 <-> L2 : TCP (stream)  (optional: L1 implements L2)
#
# Usage:
#   # L2 server
#   ruby astrojitd.rb --mode l2 --bind 0.0.0.0 --l2-port 4000 --l2-obj-dir ./l2_obj
#
#   # L1 server (connects to L2 at startup; non-blocking wait for replies)
#   ruby astrojitd.rb --mode l1 --uds /tmp/astrojit_l1.sock --l2-host 127.0.0.1 --l2-port 4000 --so-dir ./so_store --l1-obj-dir ./l1_objcache
#
#   # L1 server that also performs L2 work (no TCP to L2)
#   ruby astrojitd.rb --mode l1 --l1-implements-l2 --uds /tmp/astrojit_l1.sock --so-dir ./so_store --l2-obj-dir ./obj_store
#
# Notes:
# - Compile/link commands are intentionally simple; adjust cc/cflags/ldflags as needed.
# - hash is uint64. If outputs depend on target/flags, fold them into hash generation on L0 side.

require 'set'
require "socket"
require "fileutils"
require "optparse"
require "shellwords"
require 'monitor'

# -------------------------
# Protocol
# -------------------------
module Proto
  REQ_QUERY   =  1
  REQ_COMPILE =  2
  REQ_CTL     =  9
  RES_HIT     = 11
  RES_MISS    = 12
  RES_OBJ     = 13
  RES_ERR     = 14

  STRS = constants.grep(/^(REQ_|RES_)/){|c|
    [const_get(c), c]
  }.to_h

  HEADER_SIZE = 16 # 4(type) + 4(size) + 8(hash)

  def self.read_exact(io, n)
    buf = +""
    while buf.bytesize < n
      chunk = io.read(n - buf.bytesize)
      return nil if chunk.nil?
      buf << chunk
    end
    buf
  rescue Errno::ECONNRESET
    nil
  end

  def self.type_str(t) = Proto::STRS[t]
  def self.hash_str(h) = "%x" % [h]

  # => [type(Integer), hash(Integer uint64), payload(String binary)] or nil on EOF
  def self.recv(to, from, io)
    hdr = read_exact(io, HEADER_SIZE)
    return nil unless hdr
    type, size, h = hdr.unpack("N N Q>")
    payload = size > 0 ? read_exact(io, size) : ""
    return nil if size > 0 && payload.nil?

    warn "recv:#{from}->#{to}(#{io.fileno}) type:#{type_str(type)} h:#{hash_str(h)}"

    [type, h, payload.b]
  end

  def self.send(from, to, io, type, h, payload = +"")
    warn "send:#{from}->#{to}(#{io.fileno}) type:#{type_str(type)} h:#{hash_str(h)}"

    payload = payload.b
    io.write([type, payload.bytesize, h].pack("N N Q>"))
    io.write(payload) if payload.bytesize > 0
  rescue Errno::EPIPE
    nil
  end
end

# -------------------------
# Utility
# -------------------------
def u64_to_hex(h)
  ("%x" % h)
end

def safe_close(io)
  io.close
rescue
  nil
end

def run_command cmd
  warn "--> run command: #{cmd}"
  system(cmd)
end

# -------------------------
# L2 Implementation (obj store + compilation)
# -------------------------
class L2Impl
  def initialize(store_dir:, obj_dir:, cc:, cflags:)
    @obj_dir = File.join(store_dir, obj_dir)
    @cc = cc
    @cflags = cflags
    FileUtils.mkdir_p(@obj_dir)

    @work_q = Queue.new
    max_worker = 2
    @workers = max_worker.times.map do
      Thread.new do
        while task = @work_q.pop
          task.call
        end
      end
    end
  end

  def add_task &b
    @work_q << b
  end

  def obj_path(h)
    File.join(@obj_dir, "#{u64_to_hex(h)}.o")
  end

  def has_obj?(h)
    File.exist?(obj_path(h))
  end

  def read_obj(h)
    File.binread(obj_path(h))
  end

  # compile if it is not available
  def get_obj h, src_bytes
    if has_obj?(h)
      read_obj(h)
    else
      compile_to_obj(h, src_bytes)
    end
  end

  def compile_to_obj(h, src_bytes)
    c_path = File.join(@obj_dir, "#{u64_to_hex(h)}.c")
    o_path = obj_path(h)

    header = <<~EOS
    #include <string.h>
    #include <stdlib.h>
    #define ASTRO_SPECIALIZED 1
    #include "#{Dir.pwd}/node.h"
    #include "#{Dir.pwd}/node_eval.c"
    #define dispatch_info(...)
    EOS

    File.binwrite(c_path, header + src_bytes)

    cmd = "#{@cc} #{@cflags} -c #{Shellwords.escape(c_path)} -o #{Shellwords.escape(o_path)}"
    ok = run_command(cmd)
    raise "compile failed: #{cmd}" unless ok && File.exist?(o_path)

    File.binread(o_path)
  end
end

# -------------------------
# L2 Server (TCP)
# -------------------------
class L2Server
  def initialize(bind_host:, port:, l2_impl:)
    @bind_host = bind_host
    @port = port
    @l2 = l2_impl
  end

  def run
    srv = TCPServer.new(@bind_host, @port)
    warn "L2 listening on #{@bind_host}:#{@port}"
    loop do
      sock = srv.accept
      sock.sync = true
      Thread.new(sock) { |s| handle_client(s) }
    end
  ensure
    safe_close(srv) if srv
  end

  def handle_client(sock)
    loop do
      msg = Proto.recv(:L2, :L1, sock)
      break unless msg
      type, h, payload = msg

      case type
      when Proto::REQ_QUERY
        if @l2.has_obj?(h)
          obj = @l2.read_obj(h)
          Proto.send(:L2, :L1, sock, Proto::RES_OBJ, h, obj)
        else
          Proto.send(:L2, :L1, sock, Proto::RES_MISS, h)
        end

      when Proto::REQ_COMPILE
        @l2.add_task do
          obj = @l2.get_obj(h, payload)
          Proto.send(:L2, :L1, sock, Proto::RES_OBJ, h, obj)
        rescue => e
          Proto.send(:L2, :L1, sock, Proto::RES_ERR, h, "#{e.class}: #{e.message}\n")
        end
      else
        Proto.send(:L2, :L1, sock, Proto::RES_ERR, h, "unknown message type=#{type}\n")
      end
    end
  ensure
    safe_close(sock)
  end
end

# -------------------------
# L1 Server (UDS; connects to L2; non-blocking replies; hash-dedup)
# -------------------------
class L1Server
  # States
  Querying  = :Querying
  NotFound  = :NotFound
  Compiling = :Compiling
  Compiled  = :Compiled
  Error     = :Error

  def initialize(
    store_dir:,
    uds_path:,
    so_dir:,
    l1_obj_dir:,
    l2_host:,
    l2_port:,
    l2_impl:,
    cc:,
    ldflags:
  )
    @uds_path = uds_path
    @so_dir = File.join(store_dir, so_dir)
    @l1_obj_dir = File.join(store_dir, l1_obj_dir)
    @l2_host = l2_host
    @l2_port = l2_port
    @l2_impl = l2_impl
    @cc = cc
    @ldflags = ldflags

    if @l2_impl
      extend L2Integrated
    else
      extend L2Relay
    end

    # setup server
    FileUtils.mkdir_p(@so_dir)
    FileUtils.mkdir_p(@l1_obj_dir)

    @l2_sock = nil
    @so_cnt = 0

    @cache_state = {} # {h => States}
    @cache_state_monitor = Monitor.new
    @l0_clients = Hash.new{|h, k| h[k] = Set.new} # {io => Set[hash, ...]}
  end

  # ---------- Local code store ----------
  def so_path(h)
    File.join(@so_dir, "#{u64_to_hex(h)}.so")
  end

  def local_has_so?(h)
    File.exist?(so_path(h))
  end

  # Link obj bytes into a per-hash .so in local store
  def materialize_so_from_obj(h, obj_bytes)
    o_path = File.join(@l1_obj_dir, "#{u64_to_hex(h)}.o")
    File.binwrite(o_path, obj_bytes)

    out_so = so_path(h)
    cmd = "#{@cc} -shared #{@ldflags} #{Shellwords.escape(o_path)} -o #{Shellwords.escape(out_so)}"
    ok = run_command(cmd)
    raise "link failed: #{cmd}" unless ok && File.exist?(out_so)
    @so_cnt += 1
    true
  end

  # ---------- L2 connection management ----------
  def connect_l2_if_needed
    return if @l2_impl
    return if @l2_sock && !@l2_sock.closed?

    begin
      @l2_sock = TCPSocket.new(@l2_host, @l2_port)
      @l2_sock.sync = true
      warn "L1 connected to L2 #{@l2_host}:#{@l2_port}"
    rescue => e
      @l2_sock = nil
      warn "L1 failed to connect to L2: #{e.class}: #{e.message}"
    end
  end

  def l2_available?
    @l2_sock && !@l2_sock.closed?
  end

  module L2Integrated
    def send_l2_query(h)
      if @l2_impl.has_obj?(h)
        obj = @l2_impl.read_obj(h)
        handle_obj_from_l2(h, obj)
      else
        cache_state_to_result NotFound, h do
          reply_all(h, Proto::RES_MISS)
        end
      end
    end

    def send_l2_compile(h, src_bytes)
      @l2_impl.add_task do
        obj = @l2_impl.get_obj(h, src_bytes)
        handle_obj_from_l2(h, obj)
      rescue => e
        warn [e.message, e.backtrace].inspect
        reply_all(h, Proto::RES_ERR, "#{e.class}: #{e.message}\n")
      end
    end
  end

  module L2Relay
    def ensure_l2_connection
      connect_l2_if_needed
      if !l2_available?
        reply_all(h, Proto::RES_ERR, "L2 unavailable\n")
      else
        yield
      end
    rescue => e
      reply_all(h, Proto::RES_ERR, "send QUERY failed: #{e.class}: #{e.message}\n")
    end

    def send_l2_query h
      ensure_l2_connection do
        Proto.send(:L1, :L2, @l2_sock, Proto::REQ_QUERY, h)
      end
    end

    def send_l2_compile(h, src_bytes)
      ensure_l2_connection do
        Proto.send(:L1, :L2, @l2_sock, Proto::REQ_COMPILE, h, src_bytes)
      end
    end
  end

  # state transition [ -> NotFound, Compiled ]
  def cache_state_to_result to_result, h
    # pp [@cache_state[h], to_result]

    @cache_state_monitor.synchronize do
      case [@cache_state[h], to_result]
      in nil, _
        @cache_state[h] = to_result
        yield
      in Querying, NotFound
        @cache_state[h] = NotFound
        yield
      in Querying, Compiled
        @cache_state[h] = Compiled
        yield
      in NotFound, NotFound
        # ignore
      in NotFound, Compiled
        @cache_state[h] = Compiled
        yield
      in Compiling, NotFound
        raise "BUG: should not occuer"
      in Compiling, Compiled
        @cache_state[h] = Compiled
        yield
      in Compiled, NotFound
        raise "BUG: should not occuer"
      in Compiled, Compiled
        # ignore
      in _, Error
        @cache_state[h] = Error
        yield
      end
    end
  end

  # L2 returns OBJ for both QUERY-hit and COMPILE result
  def handle_obj_from_l2(h, obj_bytes)
    materialize_so_from_obj(h, obj_bytes) unless local_has_so?(h)

    cache_state_to_result Compiled, h do
      reply_all(h, Proto::RES_HIT)
    end
  rescue => e
    reply_all(h, Proto::RES_ERR, "link failed: #{e.class}: #{e.message}\n")
  end

  def handle_l2_readable
    msg = Proto.recv(:L1, :L2, @l2_sock)

    if msg.nil?
      warn "L1: L2 disconnected"
      safe_close(@l2_sock)
      @l2_sock = nil

      return
    end

    type, h, payload = msg

    case type
    when Proto::RES_OBJ
      handle_obj_from_l2(h, payload)
    when Proto::RES_MISS
      cache_state_to_result NotFound, h do
        reply_all(h, type)
      end
    when Proto::RES_ERR
      cache_state_to_result Error, h do
        reply_all(h, type, payload)
      end
    else
      reply_all(h, type, "unexpected L2 msg type=#{type}\n")
    end
  rescue => e
    warn "L1: error while reading L2: #{e.class}: #{e.message}"
    safe_close(@l2_sock)
    @l2_sock = nil
  end

  def reply_all(h, type, payload = +"")
    @l0_clients.each do |io, hs|
      if hs.include? h
        case type
        when Proto::RES_HIT, Proto::RES_ERR
          hs.delete h
        when Proto::RES_MISS
        when Proto::RES_OBJ
          raise "BUG: should not reply Proto::RES_OBJ to L0"
        end

        begin
          Proto.send :L1, :L0, io, type, h, payload
        rescue
          warn "L1->L0 (#{io}) error: #{$!}"
          safel_close(io)
        end
      end
    end
  end

  # state transition [ -> Querying or Compiling ]
  def cache_state_to_ing state_to, io, h
    @cache_state_monitor.synchronize do
      if local_has_so?(h)
        @cache_state[h] = Compiled
        Proto.send :L1, :L0, io, Proto::RES_HIT, h
        return
      else
        if @cache_state[h] == Compiled
          @cache_state[h] = nil
        end
      end

      # pp [@cache_state[h], state_to]
      case [@cache_state[h], state_to]
      in nil, _
        @cache_state[h] = state_to
        @l0_clients[io] << h
        yield
      in Querying, Querying
        # just ignore
      in Querying, Compiling
        @cache_state[h] = Compiling
        @l0_clients[io] << h
        yield
      in NotFound, Querying
        Proto.send(:L1, :L0, io, Proto::RES_MISS, h)
      in NotFound, Compiling
        @cache_state[h] = Compiling
        @l0_clients[io] << h
        yield
      in Compiling, _
        # just ignore
      in Compiled, _
        Proto.send(:L1, :L0, io, Proto::RES_HIT, h)
      in _, Error
        @cache_state[h] = Error
        yield
      end
    end
  end

  def close_l0 io
    @l0_clients.delete io
    warn "L0 #{io} disconnected"
    safe_close(io)
  end

  def handle_l0_readable(io)
    msg = Proto.recv(:L1, :L0, io)

    if msg.nil?
      close_l0(io)
      return
    end

    type, h, payload = msg

    # warn ('L0->L1 type:%d h:%x payload:%s' % [type, h, payload])

    case type
    when Proto::REQ_QUERY
      cache_state_to_ing Querying, io, h do
        send_l2_query(h)
      end
    when Proto::REQ_COMPILE
      cache_state_to_ing Compiling, io, h do
        send_l2_compile(h, payload)
      end
    else
      # send bakc to the L0
      Proto.send(:L1, :L0, io, Proto::RES_ERR, h, "unknown message type=#{type}\n")
    end
  rescue => e
    raise
    exit
    begin
      Proto.send(:L1, :L0, io, Proto::RES_ERR, h || 0, "#{e.class}: #{e.message}\n")
    rescue
      nil
    end
    close_l0(io)
  end

  def update_all_so
    list_path = "all_so_objs.list"
    all_so_path = File.join(@so_dir, 'all.so')
    File.write(list_path, Dir[File.join(@l1_obj_dir, '*.o')].join("\n"))
    run_command "#{@cc} -shared -o #{all_so_path} @#{list_path}"
    warn "#{all_so_path} is updated"
  end

  # ---------- Main loop ----------
  def run
    File.unlink(@uds_path) if File.exist?(@uds_path)
    srv = UNIXServer.new(@uds_path)
    warn "L1 listening on #{@uds_path}"

    # Requirement: connect L2 at startup
    connect_l2_if_needed

    prev_so_cnt = 0
    l0_clients = []

    loop do
      l0_clients.reject!{|e| e.closed?}
      ios = [srv] + l0_clients
      ios << @l2_sock if @l2_sock && !@l2_sock.closed?

      readable, = IO.select(ios, nil, nil, 3.0)

      # periodic reconnect attempt (if disconnected and not implementing L2)
      connect_l2_if_needed

      if readable
        readable.each do |io|
          case
          when io == srv
            c = srv.accept
            c.sync = true
            l0_clients << c
            warn "L0 accepts #{c}"
          when io == @l2_sock
            handle_l2_readable
          when !io.closed?
            handle_l0_readable(io)
          end
        end
      else
        # timeout
        if @so_cnt != prev_so_cnt
          prev_so_cnt = @so_cnt
          update_all_so
        end
      end
    end
  ensure
    safe_close(srv) if srv
    l0_clients.each { |io| safe_close(io) }
    safe_close(@l2_sock) if @l2_sock
    File.unlink(@uds_path) rescue nil
  end
end

# -------------------------
# CLI
# -------------------------
opts = {
  mode: nil,

  uds: "/tmp/astrojit_l1.sock",
  store_dir:  "./astrojit",
  so_dir:     "l1_so_store",
  l1_obj_dir: "l1_obj_store",

  bind: "0.0.0.0",
  l2_host: "127.0.0.1",
  l2_port: 4000,
  l2_obj_dir: "l2_obj_store",

  cc: "cc",
  cflags: "-O3 -fPIC",
  ldflags: "-fPIC",
}

OptionParser.new do |o|
  o.on("--mode MODE", "l1 (relay), l2 (compile), l12 (integrated)") { |v| opts[:mode] = v }
  o.on("--store-dir DIR",  "Default:#{opts[:store_dir]}") { |v| opts[:store_dir] = v }

  o.on("--uds PATH",       "Default:#{opts[:uds]}") { |v| opts[:uds] = v }
  o.on("--l1-so-dir DIR",  "L1 SO cache dir. Default:#{opts[:so_dir]}") { |v| opts[:so_dir] = v }
  o.on("--l1-obj-dir DIR", "L1 local obj cache dir. Default:#{opts[:l1_obj_dir]}") { |v| opts[:l1_obj_dir] = v }

  o.on("--bind HOST", "L2 bind host") { |v| opts[:bind] = v }
  o.on("--l2-host HOST", "Default:#{opts[:l2_host]}") { |v| opts[:l2_host] = v }
  o.on("--l2-port PORT", "Default:#{opts[:l2_port]}", Integer) { |v| opts[:l2_port] = v }
  o.on("--l2-obj-dir DIR", "L2 object store dir. Default:#{opts[:l2_obj_dir]}") { |v| opts[:l2_obj_dir] = v }

  o.on("--cc CC",         "Default:#{opts[:cc]}") { |v| opts[:cc] = v }
  o.on("--cflags FLAGS",  "Default:#{opts[:cflags]}") { |v| opts[:cflags] = v }
  o.on("--ldflags FLAGS", "Default:#{opts[:ldflags]}") { |v| opts[:ldflags] = v }
end.parse!

def kick_l1 opts, integrated
  l2_impl = L2Impl.new(store_dir: opts[:store_dir], obj_dir: opts[:l2_obj_dir], cc: opts[:cc], cflags: opts[:cflags]) if integrated

  L1Server.new(
    store_dir: opts[:store_dir],
    uds_path: opts[:uds],
    so_dir: opts[:so_dir],
    l1_obj_dir: opts[:l1_obj_dir],
    l2_impl: l2_impl, # nil unless integrated mode
    l2_host: opts[:l2_host],
    l2_port: opts[:l2_port],
    cc: opts[:cc],
    ldflags: opts[:ldflags]
  ).run
end

case opts[:mode]
when "l2"
  l2_impl = L2Impl.new(store_dir: opts[:store_dir], obj_dir: opts[:l2_obj_dir], cc: opts[:cc], cflags: opts[:cflags])
  L2Server.new(bind_host: opts[:bind], port: opts[:l2_port], l2_impl: l2_impl).run
when "l1"
  kick_l1 opts, false
when 'l12'
  kick_l1 opts, true
else
  abort "Usage: ruby astrojitd.rb --mode l1|l2|l12 [options]"
end
