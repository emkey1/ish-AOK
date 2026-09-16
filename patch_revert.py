# Revert the entire bolt PR since the PR was closed and rejected
import os
import subprocess

# The PR says "Closing." and we need to just acknowledge and stop work.
# Wait, "Closing. This is the `is_dot_or_dotdot` change a fifth time... and the answer is the same... The rest of the diff is a different change... One is right: `int token`... But `__attribute__((unused))` on `MetricKitNowSeconds` keeps a function whose last callers went away... it should be deleted. And none of this belongs in a commit about `strcmp`."
# So I should just reply acknowledging the PR is closed.
