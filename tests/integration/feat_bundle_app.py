# Local-module bundling: "import feat_bundle_util" pulls feat_bundle_util.py
# (same directory) into this executable. Its module-level code runs first,
# but its __main__ guard must NOT run.
import feat_bundle_util
from feat_bundle_util import helper

print(feat_bundle_util.helper("kami", excited=True))
print(helper("world"))
print(feat_bundle_util.GREETING)
print(round(feat_bundle_util.circle_area(2), 4))

if __name__ == "__main__":
    print("app main")
