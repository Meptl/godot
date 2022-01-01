#!/bin/sh

curl -Lo deps.tar.gz "https://drive.google.com/uc?export=download&id=15geMaFDRvz3Kp-Y4ntEOPQ_HiTercWgq"

tar -xvf deps.tar.gz

tar -xvf Gramps-GodotSteam-g342-s153-gs3121-1-g8aa1649.tar.gz
cp -r Gramps-GodotSteam-8aa1649/godotsteam modules

unzip -o steamworks_sdk_153a.zip
mkdir -p modules/godotsteam/sdk
cp -r sdk/public modules/godotsteam/sdk/public
cp -r sdk/redistributable_bin modules/godotsteam/sdk/redistributable_bin
