group_by(.city) | map({city: .[0].city, count: length})
