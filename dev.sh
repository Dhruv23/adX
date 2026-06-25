#!/bin/bash

# Ensure the systemd service is active (failsafe)
sudo systemctl start ollama

# Open VS Code in the current working directory
code .