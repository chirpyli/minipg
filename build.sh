if [ "$1" = "clean" ]; then
    make maintainer-clean
    exit 0
fi

make maintainer-clean

./configure --prefix=/home/postgres/minipg --enable-debug 
