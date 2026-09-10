Import("env")


def _as_list(value):
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return list(value)
    return [value]


cxxflags = _as_list(env.get("CXXFLAGS"))
required_flags = (
    "-Werror=reorder",
    "-Werror=missing-field-initializers",
    "-Werror=sign-compare",
    "-Werror=unused-variable",
)
for flag in required_flags:
    if flag not in cxxflags:
        env.Append(CXXFLAGS=[flag])
