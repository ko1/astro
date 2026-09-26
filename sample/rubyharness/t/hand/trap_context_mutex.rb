# Mutex#lock / #synchronize inside a Signal.trap handler raise ThreadError (CRuby's
# trap context); try_lock is allowed, and the context ends with the handler.
trap("USR1") { $r = (Mutex.new.synchronize { :locked } rescue [$!.class, $!.message]); $t = Mutex.new.try_lock }
Process.kill("USR1", $$); sleep 0.1
p $r, $t
p Mutex.new.synchronize { :ok }
