import re
import sys

def replace_strcmp(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # We want to insert the helper if not present
    helper = """
static inline bool is_dot_or_dotdot(const char *name) {
    if (name[0] != '.') return false;
    if (name[1] == '\\0') return true;
    if (name[1] != '.') return false;
    return name[2] == '\\0';
}
"""
    if "is_dot_or_dotdot" not in content and any(x in filepath for x in ["fake-migrate.c", "fake-snapshot.c", "fs/proc/ish.c", "FileProviderItem.m", "FileProviderEnumerator.m"]):
        # Find a good place to insert. We need a safe place, after the last #include or #import
        # We find all matches for `#include <...>` or `#include "..."` or `#import ...`
        matches = list(re.finditer(r'#(?:include|import)\s*[<"][^>"]+[>"]\n', content))
        if matches:
            last_match = matches[-1]
            insert_pos = last_match.end()
            content = content[:insert_pos] + helper + content[insert_pos:]
        else:
            content = helper + content

    # Now replace the specific usages
    content = re.sub(
        r'strcmp\(([^,]+),\s*"\."\)\s*==\s*0\s*\|\|\s*strcmp\(\1,\s*"\.\."\)\s*==\s*0',
        r'is_dot_or_dotdot(\1)',
        content
    )

    with open(filepath, 'w') as f:
        f.write(content)

for filepath in ["fs/fake-migrate.c", "fs/fake-snapshot.c", "fs/proc/ish.c", "app/FileProvider/FileProviderItem.m", "app/FileProvider/FileProviderEnumerator.m"]:
    replace_strcmp(filepath)
