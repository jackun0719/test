#!/bin/bash

# Temporary script to allow a RO DataSet image to be dropped
# into a pre-formed flashotp*.bin image for flashing.
#
# inputs: FLASHBIN_ORIG, RODATASET_FILENAME
# output: FLASHBIN_NEW
# uses: SUPPORT_DIR/makeimg.sh, SUPPORT_DIR/makeseg.sh
#

FLASHBIN_ORIG=${FLASHBIN_ORIG:-flashotp.bin}
FLASHBIN_ORIG_SZ=$(stat --format=%s $FLASHBIN_ORIG)
FLASHBIN_NEW=${FLASHBIN_NEW:-${FLASHBIN_ORIG}.new}
RODATASET_FILENAME=${RODATASET_FILENAME:-rodsetimage.bin}
PRINTF="/usr/bin/printf"

if [ $# -eq 1 ]; then
    RODATASET_FILENAME=$1
else
    echo Usage: $0 ReadOnlyDataSetImage.bin
    exit
fi

if [ \! -f "$RODATASET_FILENAME" ]; then
    echo Error: $RODATASET_FILENAME is not a file
    exit
fi

if [ \! -r "$RODATASET_FILENAME" ]; then
    echo Error: Cannot access $RODATASET_FILENAME
    exit
fi

if [ -x ./makeimg.sh -a -x ./makeseg.sh ]; then
    SUPPORT_DIR=.
else
    SUPPORT_DIR=${SUPPORT_DIR:-../../support}
fi

# Read a particular word from a file
read_word()
{
  filename=$1
  offset=$2

  echo 0x$(dd if=$1 bs=1 count=4 skip=$offset 2> /dev/null | od -t x4 | head -1 | cut -d " " -f 2)
}

# Replace a particular word in a file with a specified word
write_word()
{
  filename=$1
  offset=$2
  word=$3

  dd if=$filename bs=1 count=$offset > ${filename}.tmp 2> /dev/null
  word32=$($PRINTF "%08x\n" $word)

  byte0=$(echo $word32 | cut -b 7-8)
  byte1=$(echo $word32 | cut -b 5-6)
  byte2=$(echo $word32 | cut -b 3-4)
  byte3=$(echo $word32 | cut -b 1-2)

  $PRINTF "\x$byte0" >> ${filename}.tmp
  $PRINTF "\x$byte1" >> ${filename}.tmp
  $PRINTF "\x$byte2" >> ${filename}.tmp
  $PRINTF "\x$byte3" >> ${filename}.tmp

  dd if=$filename bs=1 skip=$(($offset + 4)) >> ${filename}.tmp 2> /dev/null
  cp ${filename}.tmp $filename

  rm -f ${filename}.tmp
}

sizeof_flash_segment_hdr=12
flash_desc=$(read_word $FLASHBIN_ORIG  $((8*4)))
# echo flash_desc=$flash_desc
flash_desc_sz=$((8*4)) # 8 words in flash_descriptor
cfg_hdr_sz=$((9*4))
mem_addr=$(read_word $FLASHBIN_ORIG  $(($FLASHBIN_ORIG_SZ - 28)))
mem_addr=$($PRINTF "%x" $mem_addr)
# echo mem_addr=$mem_addr

# Strip out termination
dd if=$FLASHBIN_ORIG of=$FLASHBIN_NEW bs=1 count=$((FLASHBIN_ORIG_SZ - 28)) 2> /dev/null

# Append ReadOnly DataSets
seg_length=$(stat --format=%s $RODATASET_FILENAME)
if [ $seg_length -gt 65536 ]; then
    echo Error: $RODATASET_FILENAME is too large
    rm $FLASHBIN_NEW
    exit
fi

# echo ROdset length is $seg_length

# flash_addr must match FLASH_RODSET_START in make_flash.sh
#flash_addr=0x20000 # Since v2_iot patches size exceeded, moving the DSET boundary a little.

flash_addr=0x2d000

${SUPPORT_DIR}/makeimg.sh \
        -out seg_hdr \
        -new \
        -word 0x48534c46 \
        -word $flash_addr \
        -word $seg_length
cat seg_hdr $RODATASET_FILENAME > seg
${SUPPORT_DIR}/makeseg.sh \
        -out $FLASHBIN_NEW \
        -nc \
        -data seg $mem_addr
mem_addr=$($PRINTF "%x" $((0x$mem_addr + $seg_length + $sizeof_flash_segment_hdr)))

# Append termination
flash_addr=0
seg_length=0
${SUPPORT_DIR}/makeimg.sh \
        -out seg_hdr \
        -new \
        -word 0x48534c46 \
        -word $flash_addr \
        -word $seg_length
${SUPPORT_DIR}/makeseg.sh \
        -o $FLASHBIN_NEW \
        -nc \
        -data seg_hdr $mem_addr \
        -done
rm -f seg_hdr seg

# Adjust total length, first word of cfg_hdr
FLASHBIN_NEW_SZ=$(($(stat --format=%s $FLASHBIN_NEW) - $cfg_hdr_sz))
write_word $FLASHBIN_NEW  0  $FLASHBIN_NEW_SZ
echo New binary created: $FLASHBIN_NEW
