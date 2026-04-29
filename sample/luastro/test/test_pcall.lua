-- pcall and error handling.
-- Note: luastro reports plain error messages (no file:line prefix), so
-- this test stores the error text in a variable and only inspects fields
-- that match across implementations.

local ok, err = pcall(function() error("boom") end)
print(ok, type(err) == "string")

local ok, val = pcall(function() return 42 end)
print(ok, val)

-- nested pcall: inner error doesn't escape
local ok, msg = pcall(function()
  local _ = pcall(function() error("inner") end)
  return "outer survived"
end)
print(ok, msg)
