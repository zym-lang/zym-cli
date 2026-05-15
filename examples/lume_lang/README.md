# lume

a small scripting language. files end in `.lm`.

```
lume yourscript.lm
```

## the basics

```
var x = 10
var name = "world"
print("hello " + name)
```

functions:

```
fn greet(name)
  print("hi " + name)
end
```

control flow is `if / else / end`, loops are `while` and `for x in list`. maps and arrays exist. closures work.

## builtins

`print`, `str`, `num`, `length`, `push`, `pop`, `range`, `charAt`, `slice`, `contains`, `fileRead`, `typeof`, `clock`

## that's it

it's a tree-walk interpreter. it runs. don't stress it too hard.

## running lume

requires the [Zym](https://zym-lang.org) runtime.

run directly:

    zym lume.zym -- yourscript.lm

or build a standalone binary:

    zym lume.zym -o lume
    zym lume.zym -o lume.exe

then just:

    lume yourscript.lm
