#!/bin/bash

prev_aplus=""
prev_aminus=""
change_count=0
iteration=0

while [ $change_count -lt 10 ]; do
  iteration=$((iteration + 1))
  data=$(curl -s http://power3/json 2>/dev/null | jq -r '.energy | "\(.aplus) \(.aminus)"' 2>/dev/null)
  
  if [ -z "$data" ]; then
    echo "Error: Could not fetch data"
    sleep 1
    continue
  fi
  
  aplus=$(echo "$data" | awk '{print $1}')
  aminus=$(echo "$data" | awk '{print $2}')
  
  if [ -n "$prev_aplus" ]; then
    if [ "$aplus" != "$prev_aplus" ] || [ "$aminus" != "$prev_aminus" ]; then
      change_count=$((change_count + 1))
      echo "[$iteration, changes: $change_count] A+ = $aplus Wh, A- = $aminus Wh"
    else
      echo "[$iteration] A+ = $aplus Wh, A- = $aminus Wh (no change)"
    fi
  else
    echo "[$iteration] A+ = $aplus Wh, A- = $aminus Wh (initial)"
  fi
  
  prev_aplus="$aplus"
  prev_aminus="$aminus"
  sleep 3
done

echo "Done: detected 10 value changes"
