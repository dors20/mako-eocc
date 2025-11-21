# EC2 Connection Guide - Managing Changing IPs

## Understanding the Setup

**Important**: Cursor chat runs on your **local machine**, not on the EC2 instance. When you SSH into EC2, you're just accessing files remotely. The chat interface is always available in your local Cursor app.

## Solutions for Changing EC2 IPs

### Option 1: Use Elastic IP (Recommended)
Assign a static Elastic IP to your EC2 instance so the IP never changes:

```bash
# In AWS Console:
# 1. Go to EC2 → Elastic IPs → Allocate Elastic IP address
# 2. Select your instance → Actions → Networking → Associate Elastic IP address
# 3. Choose your Elastic IP and associate it
```

### Option 2: Use SSH Config with Dynamic IP Lookup
Create/update `~/.ssh/config` on your local machine:

```ssh-config
Host mako-ec2
    HostName <current-ip-or-dns>
    User ubuntu
    IdentityFile ~/.ssh/your-key.pem
    StrictHostKeyChecking no
    UserKnownHostsFile /dev/null
```

Then update the IP when it changes, or use a script to fetch it dynamically.

### Option 3: Use EC2 Instance Connect or Session Manager
- **EC2 Instance Connect**: Access via AWS Console (no IP needed)
- **AWS Systems Manager Session Manager**: SSH without managing IPs/keys

### Option 4: Use a Dynamic DNS Script
Create a script that updates your connection when IP changes:

```bash
#!/bin/bash
# update_ec2_ip.sh
NEW_IP=$(aws ec2 describe-instances --instance-ids i-xxxxx --query 'Reservations[0].Instances[0].PublicIpAddress' --output text)
sed -i "s/HostName .*/HostName $NEW_IP/" ~/.ssh/config
```

### Option 5: Use Cursor's Remote SSH Feature
If using Cursor's built-in remote SSH:
1. Install "Remote - SSH" extension (if not already)
2. Use `ssh://ubuntu@<ip>` or your SSH config alias
3. When IP changes, just update the connection string

## Quick Reference

**To connect via SSH:**
```bash
ssh -i ~/.ssh/your-key.pem ubuntu@<ec2-ip>
```

**To find current EC2 IP:**
```bash
# From AWS CLI
aws ec2 describe-instances --instance-ids i-xxxxx --query 'Reservations[0].Instances[0].PublicIpAddress' --output text

# Or from within EC2
curl http://169.254.169.254/latest/meta-data/public-ipv4
```

## Best Practice
Use **Elastic IP** - it's free when attached to a running instance and prevents IP changes.

