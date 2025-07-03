# naruby: Not a Ruby

## Build

### libprism

* pull submodule: `git pull` or `git submodule update`
* apply the follwoing patch to the prism

```
diff --git a/include/prism/util/pm_constant_pool.h b/include/prism/util/pm_constant_pool.h
index 6df23f8f..f25063a8 100644
--- a/include/prism/util/pm_constant_pool.h
+++ b/include/prism/util/pm_constant_pool.h
@@ -161,7 +161,7 @@ bool pm_constant_pool_init(pm_constant_pool_t *pool, uint32_t capacity);
  * @param constant_id The id of the constant to get.
  * @return A pointer to the constant.
  */
-pm_constant_t * pm_constant_pool_id_to_constant(const pm_constant_pool_t *pool, pm_constant_id_t constant_id);
+PRISM_EXPORTED_FUNCTION pm_constant_t * pm_constant_pool_id_to_constant(const pm_constant_pool_t *pool, pm_constant_id_t constant_id);

 /**
  * Find a constant in a constant pool. Returns the id of the constant, or 0 if
```

* `bundle install`
* `bundle exec rake`

and you can get `prism/build/libprism.so`.

### naruby

* run `make` and you can make an interpreter. `make run` will run `./test.na.rb`.

```ruby
# test.na.rb

p 0

__END__
...
```

and run `make run` will build `./naruby` and run it like:

```
./naruby  test.na.rb
(node_seq (node_lset 0 (node_num 0)) (node_call "p" 1 0 <cc>))
p:0
Result: 0, node_cnt:7
generate_sc_entry - name:main func:SD_d0941f9e0749e5be
generate_sc_entry - name:p func:SD_23175c51e511a3ef
generate_sc_entry - name:zero func:SD_5235c743aaad45bd
generate_sc_entry - name:bf_add func:SD_905b2ac5ff0f29aa
```

In this case, `./test.na.rb` is compiled and `./node_specialized.c` is generated at the end of the program (`test.na.rb`).

After that, next `make run` (or simply `make`) will build `./naruby` with generated `./node_specialized.c`.

See `Makefile` for further usage.

