savedcmd_/home/s1lencef/myfs/module/myfs.mod := printf '%s\n'   myfs.o | awk '!x[$$0]++ { print("/home/s1lencef/myfs/module/"$$0) }' > /home/s1lencef/myfs/module/myfs.mod
