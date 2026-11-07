# Source this helper from a project adapter before invoking a third-party Make.
# Native Kbuild runs with -rR; that policy must not disable upstream Make's
# suffix rules/default variables. Keep parallelism and all other Make options,
# including the inherited jobserver descriptor, while dropping Kbuild's r/R and
# parent command-line variable overrides.
reliefos_upstream_make_environment() {
    _reliefos_makeflags=${MAKEFLAGS-}
    _reliefos_makeflags=${_reliefos_makeflags%% -- *}

    case $- in
        *f*) _reliefos_restore_noglob=0 ;;
        *) _reliefos_restore_noglob=1; set -f ;;
    esac
    set -- $_reliefos_makeflags
    if [ "$_reliefos_restore_noglob" -eq 1 ]; then set +f; fi

    _reliefos_clean_makeflags=
    for _reliefos_flag do
        _reliefos_short_options=
        _reliefos_option_prefix=
        case $_reliefos_flag in
            --*) ;;
            -*)
                _reliefos_candidate=${_reliefos_flag#-}
                case $_reliefos_candidate in
                    ''|*[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz]*) ;;
                    *) _reliefos_short_options=$_reliefos_candidate
                       _reliefos_option_prefix=- ;;
                esac
                ;;
            *)
                case $_reliefos_flag in
                    ''|*[!ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz]*) ;;
                    *) _reliefos_short_options=$_reliefos_flag ;;
                esac
                ;;
        esac

        if [ -n "$_reliefos_short_options" ]; then
            _reliefos_kept_options=
            while [ -n "$_reliefos_short_options" ]; do
                _reliefos_option=${_reliefos_short_options%"${_reliefos_short_options#?}"}
                case $_reliefos_option in
                    r|R) ;;
                    *) _reliefos_kept_options=$_reliefos_kept_options$_reliefos_option ;;
                esac
                _reliefos_short_options=${_reliefos_short_options#?}
            done
            if [ -n "$_reliefos_kept_options" ]; then
                _reliefos_flag=$_reliefos_option_prefix$_reliefos_kept_options
            else
                _reliefos_flag=
            fi
        fi

        if [ -n "$_reliefos_flag" ]; then
            if [ -n "$_reliefos_clean_makeflags" ]; then
                _reliefos_clean_makeflags="$_reliefos_clean_makeflags $_reliefos_flag"
            else
                _reliefos_clean_makeflags=$_reliefos_flag
            fi
        fi
    done

    MAKEFLAGS=$_reliefos_clean_makeflags
    export MAKEFLAGS
    unset MAKEOVERRIDES
    unset _reliefos_makeflags _reliefos_restore_noglob _reliefos_clean_makeflags \
        _reliefos_flag _reliefos_short_options _reliefos_option_prefix \
        _reliefos_candidate _reliefos_kept_options _reliefos_option
}
