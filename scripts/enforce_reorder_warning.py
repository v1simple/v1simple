Import("env")


def _as_list(value):
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    return [value]


cxxflags = _as_list(env.get("CXXFLAGS"))
if "-Werror=reorder" not in cxxflags:
    env.Append(CXXFLAGS=["-Werror=reorder"])
