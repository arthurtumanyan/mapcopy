# mapcopy
File copy/generation utility using ZeroCopy approach

USAGE:

        Usage: mapcopy --source   | -s <filename> --destination | -d <filename> --sendfile | -x --debug | -D
               mapcopy --source   | -s <filename> --destination | -d <filename> --parallel | -p --debug | -D
               mapcopy --source   | -s <filename> --verify | -v
               mapcopy --generate | -g            --destination | -d <filename> --size | -S <bytes> --debug | -D
               
  
        ./mapcopy -g -d test -S 3000000 -D


        Limit:  3000000 bytes (2929 KB)
        Copied: 3000000 bytes (2929 KB)
        Rest:   0 bytes (0 KB)
        Data processing took 0.14577 seconds
