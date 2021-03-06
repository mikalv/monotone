mtn_setup()

-- Make sure we test the monotone source for mtnopt, not whatever the
-- user happens to have in PATH
copy (initial_dir .. "/mtnopt", "mtnopt")

normalized_testroot = normalize_path (test.root)

-- Escape regexp special characters to form a proper regexp that correctly
-- checks for the given path.
escaped_testroot = string.gsub(normalized_testroot, "([*+.()[\\^$|?])",
  function (x) return "\\" .. x end)

-- check default operation

-- MinGW does not process the shebang in mtnopt; must invoke sh directly
-- Vista will probably need to skip this test
-- Don't pass the full /bin/sh path, it looks like that doesn't always
-- work under mingw.
check({"sh", "./mtnopt"}, 0, true)
check(qgrep('^MTN_database="' .. escaped_testroot .. '/test.db";$', "stdout"))
check(qgrep('^MTN_branch="testbranch";$', "stdout"))

-- check operation with a specific key and just returning the value
check({'sh', './mtnopt', '-v', '-kbranch'}, 0, true)
check(not qgrep('^' .. normalized_testroot .. '/test.db$', "stdout"))
check(qgrep('^testbranch$', "stdout"))

