-- The following hook informs the buildbot about a new revision received
-- via netsync. This is done via the `buildbot sendchange` command here and
-- with the PBChangeSource on the buildbot server side.
-- 
--
-- Version history:
-- ----------------
-- 
-- 0.1 (2007-07-10) Markus Schiltknecht <markus@bluegap.ch>
--     - initial version
-- 0.2 (2011-03-11) Richard Levitte <richard@levitte.org>
--     - updated to have things more protected
-- 0.3 (2012-04-10) Markus Wanner <markus@bluegap.ch>
--     - adapt to buildbot 0.8.3 and newer
--
-- License: GPL
--
----------------------------------------------------------------------
-- To configure this hooks, use the following variables:
--
-- MB_buildbot_bin	The buildbot binary.
--                      Defaults to "buildbot"
-- MB_buildbot_master	The address:port to the buildbot master
--                      Defaults to "localhost:9989"
--

do
   local buildbot_bin = "buildbot"
   if MB_buildbot_bin then buildbot_bin = MB_buildbot_bin end

   local buildbot_master = "localhost:9989"
   if MB_buildbot_master then buildbot_master = MB_buildbot_master end
   
   local notify_buildbot =
      function (rev_id, revision, certs)
	 local author = ""
	 local changelog = ""
	 local branch = ""
	 for i,cert in pairs(certs) do 
	    if cert["name"] == "changelog" then
	       changelog = changelog .. cert["value"] .. "\n"
	    elseif cert["name"] == "author" then
	       -- we simply override the author, in case there are multiple
	       -- author certs.
	       author = cert["value"]
	    elseif cert["name"] == "branch" then
	       -- likewise with the branch cert, which probably isn't that
	       -- clever...
	       branch = cert["value"]
	    end
	 end

	 local touched_files = ""
	 for i,row in ipairs(parse_basic_io(revision)) do
	    local key = row["name"]
	    if ((key == 'delete') or (key == 'add_dir')
	        or (key == 'add_file') or (key == 'patch')) then
	       local filename = row["values"][1]
	       touched_files = touched_files .. filename .. " "
	    end
	 end

    -- Note: for buildbot versions before 0.8.3, you need to give a
    -- 'username' argument instead of 'auth' and 'who'.
	 print("monotone-buildbot-notification: Running script:",
	       buildbot_bin, "sendchange",
	       "--master", buildbot_master,
	       "--auth", "change:changepw",
	       "--who", "'"..author.."'",
	       "--revision", rev_id,
	       "--comments", "'"..changelog.."'",
	       "--branch", branch,
	       touched_files)
	 execute(buildbot_bin, "sendchange",
		 "--master", buildbot_master,
		 "--auth", "change:changepw",
		 "--who", "'"..author.."'",
		 "--revision", rev_id,
		 "--comments", changelog,
		 "--branch", branch,
		 touched_files)
      end

   local old_node_commit = note_commit
   function note_commit (new_id, revision, certs)
      if old_note_commit then
	 old_note_commit(new_id, revision, certs)
      end
      notify_buildbot(new_id, revision, certs)
   end

   push_hook_functions(
      {
	 revision_received =
	    function (new_id, revision, certs, session_id)
	       notify_buildbot(new_id, revision, certs)
	       return "continue",nil
	    end
      })
end
