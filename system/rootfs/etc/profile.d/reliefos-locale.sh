# Import this account's locale.conf as validated data for POSIX login shells
# and the XDM session. The helper emits only known locale keys and safe values.
if test -x /usr/bin/reliefsettings; then
    reliefos_locale_values=$(/usr/bin/reliefsettings --session-environment) || reliefos_locale_values=
    for reliefos_locale_value in $reliefos_locale_values; do
        case $reliefos_locale_value in
            LANG=*|LC_ALL=*|LC_CTYPE=*|LC_NUMERIC=*|LC_TIME=*|LC_COLLATE=*|LC_MONETARY=*|LC_MESSAGES=*)
                export "$reliefos_locale_value" ;;
        esac
    done
    unset reliefos_locale_value reliefos_locale_values
fi
