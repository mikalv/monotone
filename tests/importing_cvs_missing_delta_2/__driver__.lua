
mtn_setup()

-- This is the same repository as in 'importing_cvs_files', except that
-- the delta for file foo @ 1.3 as been manually removed.

check(get("cvs-repository"))

-- try an import...
check(mtn("--ticker", "none", "--branch=testbranch", "cvs_import", "cvs-repository/test"), 0, false, true)
check(qgrep("delta text for RCS version 1.3 is missing from file foo,v", "stderr"))
