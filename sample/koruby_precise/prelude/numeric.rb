# Numeric — generic #coerce for custom Numeric subclasses.  Integer/Float/Rational
# have their own (C) #coerce; this is the fallback now that Numeric is a real class.
class Numeric
  def coerce(other)
    return [other, self] if other.instance_of?(self.class)
    [Float(other), Float(self)]
  end
end
