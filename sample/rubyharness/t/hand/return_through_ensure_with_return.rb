# A block's `return` unwinding through an ensure that itself runs a plain
# method-level `return` still returns from the block's home method (rspec-core's
# run_specs returned 0 instead of failure_exit_code).
class R
  def helper = (return :h)          # a plain method-level return
  def wsh; begin; yield; ensure; helper; end; end
  def run
    x = wsh do
      return 15
    end
    [:fell_through, x]
  end
end
p R.new.run
def m2; [1].each { begin; return :from_block; ensure; R.new.helper; end }; :fell; end
p m2
