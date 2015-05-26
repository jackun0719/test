#!/bin/bash
# Add a DataSet to the ReadOnly DataSet Image.
# Parameters:
#  $1 = dset_id
#  $2 = name of file containing DataSet bytes
#  $3 = name of RO DataSet Image to append to

# Needed for makeimg.sh and makeseg.sh
AR6002_REV=${AR6002_REV:-7}
export AR6002_REV
PRINTF="/usr/bin/printf"

# Append a 32-bit word to the output file.
add_word()
{
    word32=$($PRINTF "%08x\n" $1)

        byte0=$(echo $word32 | cut -b 7-8)
        byte1=$(echo $word32 | cut -b 5-6)
        byte2=$(echo $word32 | cut -b 3-4)
        byte3=$(echo $word32 | cut -b 1-2)

        $PRINTF "\x$byte0" >> $out_file
        $PRINTF "\x$byte1" >> $out_file
        $PRINTF "\x$byte2" >> $out_file
        $PRINTF "\x$byte3" >> $out_file
}

add_string()
{
    $PRINTF "%-32s" $1 >> $out_file
}

# Start of script
filename=${filename:-"filelist"}
RODATASET_FILENAME=${RODATASET_FILENAME:-"rodsetimage.bin"}

#echo Calling $0

if [ -f $filename ]; then
    out_file=mapfile
    filesz=0
    dset_id=0x401    # Start Dataset ID for Static HTML pages 
    dbdset_id=0x421  # Start Dataset ID for DB for HTML pages

    nooffiles=$(wc -l $filename | awk '{print $1}')
    echo No of files:$nooffiles
    if [ $nooffiles -ge 8 ]; then
        echo Error: Max 7 files are supported
        exit
    fi

    # Delete the existing file
    rm -f $out_file

    # Create a mapfile
    cat $filename | while read a; do \
                            add_string $a; \
                            add_word $dset_id; \
                            dset_id=$(( $dset_id + 1 )); \
                            add_word $dbdset_id; \
                            dbdset_id=$(( $dbdset_id + 1 ));\
                            done

    # Now create a Dset for files
    dset_id=401

    # Delete the previous RODSET
    rm -f $RODATASET_FILENAME

    # Creating dataset for map file
    ./mkrodset.sh 0x400 $out_file $RODATASET_FILENAME
    cat $filename | while read a; do \
                            ./mkrodset.sh 0x$dset_id $a $RODATASET_FILENAME; \
                            dset_id=$(( $dset_id + 1 )); \
                            done

    rm -f $out_file

    # Now Insert the RODSET into the final flashotp.bin
    ./dset_insert.sh $RODATASET_FILENAME
else
    echo "File \"filelist\"  not exist"
fi
