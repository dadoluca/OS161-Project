def dbos161
  dir ../os161-base-2.0.3/kern/compile/DUMBVM
  target remote unix:.sockets/gdb
end
def dbos161h
  dir ../os161-base-2.0.3/kern/compile/HELLO
  target remote unix:.sockets/gdb
end
dbos161
