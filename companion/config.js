// Fuchey companion — public marketplace config, used by the hosted page.
// Both values are public by design: the publishable key can only read what
// the database's RLS allows (published wearables, characters, saved looks).
// A gitignored config.local.js, if present, is used instead.
// Never put the service-role / secret key here.
export default {
  SUPABASE_URL: "https://riwktpbxohkftcgidkkt.supabase.co",
  SUPABASE_ANON_KEY: "sb_publishable_5Kcelu6kbuRUFu9BUOt-FA_rnVpa4qk",
};
