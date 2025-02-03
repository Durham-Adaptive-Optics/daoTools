g++ -c main.cpp -I$DAOROOT/include -O3
g++ main.o -o record -L$DAOROOT/lib64 -ldao -lnuma -ldaoNuma -ldaoProto -lzmq -lprotobuf -lcfitsio -lyaml-cpp
rm *.fits &> /dev/null