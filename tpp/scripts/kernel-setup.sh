#!/bin/bash

COLLOID_TPP_HOME='/proj/prismgt-PG0/vrao79/colloid-tb/tpp'

set -e
set -x

sudo apt-get update

sudo apt install -y build-essential libncurses-dev bison flex libssl-dev libelf-dev fakeroot
sudo apt install -y dwarves
sudo apt install -y libnuma-dev

cd $COLLOID_TPP_HOME

cd linux-6.3
cp /boot/config-$(uname -r) .config
make olddefconfig

CONFIG_FILE=".config"
NEW_VALUE="-colloid"

# Ensure the file exists
if [[ ! -f $CONFIG_FILE ]]; then
    echo "Error: $CONFIG_FILE not found!"
    exit 1
fi

# Update CONFIG_LOCALVERSION if it exists, otherwise add it
if grep -q "^CONFIG_LOCALVERSION=" "$CONFIG_FILE"; then
    sed -i "s/^CONFIG_LOCALVERSION=.*/CONFIG_LOCALVERSION=\"$NEW_VALUE\"/" "$CONFIG_FILE"
else
    echo "CONFIG_LOCALVERSION=\"$NEW_VALUE\"" >> "$CONFIG_FILE"
fi

echo "Updated CONFIG_LOCALVERSION in $CONFIG_FILE"

./scripts/config --disable SYSTEM_TRUSTED_KEYS
./scripts/config --disable SYSTEM_REVOCATION_KEYS

make -j32 bzImage
make -j32 modules
sudo make modules_install
sudo make install

GRUB_CONFIG="/etc/default/grub"
NEW_KERNEL="1>Ubuntu, with Linux 6.3.0-colloid"

# Ensure the file exists
if [[ ! -f $GRUB_CONFIG ]]; then
    echo "Error: $GRUB_CONFIG not found!"
    exit 1
fi

# Backup the original file
sudo cp "$GRUB_CONFIG" "$GRUB_CONFIG.bak"

# Update GRUB_DEFAULT setting
if grep -q "^GRUB_DEFAULT=" "$GRUB_CONFIG"; then
    sudo sed -i "s|^GRUB_DEFAULT=.*|GRUB_DEFAULT=\"$NEW_KERNEL\"|" "$GRUB_CONFIG"
else
    echo "GRUB_DEFAULT=\"$NEW_KERNEL\"" >> "$GRUB_CONFIG"
fi

sudo update-grub
sudo reboot
