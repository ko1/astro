# 最小 'time' shim: require を通すためのもの。Time.parse / Date 連携は未対応。
class Time
  def iso8601(fraction_digits = 0)
    strftime("%Y-%m-%dT%H:%M:%S") + (utc? ? "Z" : strftime("%z").insert(3, ":"))
  end
  alias xmlschema iso8601
end
