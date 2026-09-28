#!/bin/sh
# Stand-in for curl in the ra_test --flow runs (MINICAST_RA_CURL): answers
# the RetroAchievements requests with canned JSON and appends every "r=" it
# served to $FAKE_CURL_LOG, so the test can check what reached the "server".
# Output format matches `curl -w '\n%{http_code}'`: body, newline, status.

url=""
post=0
for a in "$@"; do
	url="$a"
	[ "$a" = "--data-binary" ] && post=1
done
body=""
[ $post = 1 ] && body=$(cat)
req=$(printf '%s&%s' "$url" "$body" | tr '?&' '\n\n' | sed -n 's/^r=//p' | head -n 1)
[ -n "$FAKE_CURL_LOG" ] && echo "$req" >> "$FAKE_CURL_LOG"

case "$req" in
login2)
	printf '{"Success":true,"User":"tester","Token":"faketoken","Score":100,"SoftcoreScore":200,"Messages":0}' ;;
achievementsets)
	printf '{"Success":true,"GameId":3456,"Title":"Fake Game","ConsoleId":40,"ImageIconUrl":"https://media.retroachievements.org/Images/000001.png","RichPresenceGameId":3456,"RichPresencePatch":"","Sets":[{"AchievementSetId":1,"GameId":3456,"Title":"Fake Game","Type":"core","ImageIconUrl":"https://media.retroachievements.org/Images/000001.png","Achievements":['
	printf '{"ID":101,"Title":"Byte","Description":"RA 0x100 becomes 1","Flags":3,"Points":5,"MemAddr":"0xH000100=1","Author":"t","BadgeName":"00001","Created":1600000000,"Modified":1600000000,"Type":"","Rarity":50.0,"RarityHardcore":40.0},'
	printf '{"ID":102,"Title":"Dword","Description":"RA 0x200 becomes 0x11223344 (little-endian)","Flags":3,"Points":10,"MemAddr":"0xX000200=287454020","Author":"t","BadgeName":"00002","Created":1600000000,"Modified":1600000000,"Type":"","Rarity":50.0,"RarityHardcore":40.0}'
	printf '],"Leaderboards":[]}]}' ;;
startsession)
	printf '{"Success":true,"Unlocks":[],"HardcoreUnlocks":[],"ServerNow":1700000000}' ;;
awardachievement)
	sleep 1   # slow server: shutdown must wait for this
	id=$(printf '%s' "$body" | tr '&' '\n' | sed -n 's/^a=//p')
	printf '{"Success":true,"Score":100,"SoftcoreScore":215,"AchievementID":%s,"AchievementsRemaining":5}' "$id" ;;
*)
	printf '{"Success":true}' ;;
esac
printf '\n200'
