(.[].active) |= true | [.[] | select(.active)] | length
