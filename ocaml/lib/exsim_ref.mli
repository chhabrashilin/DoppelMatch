(** A purely functional reference model of the exchange's matching rules.

    A second, independent implementation of the semantics of [include/exsim/order_book.hpp], written for
    clarity rather than speed. It is differentially tested against the C++ engine through a shared text
    format ([scripts/ocaml_diff.sh]): both must emit identical events for the same command stream. *)

type side = Buy | Sell
type ord_type = Limit | Market
type tif = Day | Ioc | Fok

(** Self-trade prevention: what happens when an incoming order meets a resting order of the same owner. *)
type stp =
  | Stp_none  (** allow the trade *)
  | Cancel_resting  (** cancel the resting order and keep matching *)
  | Cancel_incoming  (** stop, and cancel the incoming order's remainder *)
  | Decrement_cancel
      (** cancel the smaller of the two and reduce the larger by its size; cancel both if equal *)

type reason =
  | No_reason
  | Invalid_order_id
  | Invalid_qty
  | Invalid_price
  | Duplicate_order_id
  | Unknown_order
  | Unknown_symbol
  | Would_cross
  | Book_full
  | User_canceled
  | Ioc_expired
  | Fok_unfilled
  | Self_trade

type request = New_order | Cancel | Modify

type command =
  | New of {
      id : int;
      side : side;
      price : int;  (** ignored for market orders *)
      qty : int;
      ord_type : ord_type;
      tif : tif;
      post_only : bool;
      stp : stp option;  (** per-order override of the book's default *)
      owner : int;
    }
  | Cancel_order of { id : int }
  | Modify_order of { id : int; price : int; qty : int }
      (** a size decrease at the same price keeps priority; anything else is cancel/replace *)

type event =
  | Accepted of { id : int; side : side; price : int; qty : int }
  | Rejected of { id : int; side : side; request : request; reason : reason }
  | Trade of { taker : int; maker : int; side : side; price : int; qty : int; leaves : int }
      (** [side] is the taker's; [leaves] is the maker's remaining quantity *)
  | Canceled of { id : int; side : side; price : int; qty : int; reason : reason }
  | Modified of { id : int; side : side; price : int; qty : int; reason : reason }

(** The book accepts prices in [\[min_price, min_price + num_levels)] and holds at most [max_orders]. *)
type config = { min_price : int; num_levels : int; max_orders : int; default_stp : stp }

(** An immutable order book. *)
type t

val create : config -> t

(** [apply book command] is the book after the command and the events it produced, in order. *)
val apply : t -> command -> t * event list

(** Structural invariants: never crossed, no empty levels, the id index agrees with the queues. *)
val invariants : t -> bool

(** {1 Canonical text encoding, shared with the C++ tool [exsim_difffeed]} *)

val string_of_event : event -> string

(** ["N id side price qty ord_type tif flags owner"], ["X id"] or ["U id price qty"]; [None] otherwise. *)
val command_of_string : string -> command option

(** ["CONFIG min_price num_levels max_orders stp"]. *)
val config_of_string : string -> config option

val string_of_config : config -> string
