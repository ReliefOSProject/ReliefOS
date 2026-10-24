"""Shared US-keyboard encoding for the X11 QMP acceptance tools."""


def text_keys(text: str) -> tuple[str, ...]:
    special = {
        " ": "spc",
        ".": "dot",
        "/": "slash",
        "\\": "backslash",
        "=": "equal",
        "-": "minus",
        "_": "shift-minus",
        ":": "shift-semicolon",
        # QEMU's HMP key name for the pipe key is the shifted backslash key.
        # `bar` is accepted by some builds but does not inject a character on
        # the guest keyboard layout used by ReliefOS.
        "|": "shift-backslash",
        "&": "shift-7",
    }
    keys: list[str] = []
    for character in text:
        if "A" <= character <= "Z":
            keys.append(f"shift-{character.lower()}")
        else:
            keys.append(special.get(character, character))
    return tuple(keys)
