#ifndef __FAST5_SUMMARY_HPP
#define __FAST5_SUMMARY_HPP

#include <array>
#include <string>
#include <vector>
#include <memory>

#ifndef H5_HAVE_THREADSAFE
#include <mutex>
#endif

#include "Pore_Model.hpp"
#include "State_Transitions.hpp"
#include "Event.hpp"
#include "fast5.hpp"
#include "alg.hpp"

template < typename Float_Type, unsigned Kmer_Size >
class Fast5_Summary
{
public:
    typedef Pore_Model< Float_Type, Kmer_Size > Pore_Model_Type;
    typedef Pore_Model_Dict< Float_Type, Kmer_Size > Pore_Model_Dict_Type;
    typedef Pore_Model_Parameters< Float_Type > Pore_Model_Parameters_Type;
    typedef Event< Float_Type, Kmer_Size > Event_Type;
    typedef Event_Sequence< Float_Type, Kmer_Size > Event_Sequence_Type;
    typedef State_Transition_Parameters< Float_Type > State_Transition_Parameters_Type;

    std::string file_name;
    std::string base_file_name;
    std::string read_id;
    std::string bc_grp;
    std::array< std::array< std::string, 2 >, 3 > preferred_model;
    std::map< std::array< std::string, 2 >, Pore_Model_Parameters_Type > pm_params_m;
    std::map< std::array< std::string, 2 >, std::array< State_Transition_Parameters_Type, 2 > > st_params_m;
    std::array< unsigned, 4 > strand_bounds;
    std::array< Float_Type, 2 > time_length;
    unsigned num_ed_events;
    Float_Type sampling_rate;
    Float_Type abasic_level;
    bool valid;
    bool scale_strands_together;

    // from fast5 file
    std::unique_ptr< std::vector< fast5::EventDetection_Event_Entry > > ed_events_ptr;
    // filtered
    std::array< std::unique_ptr< Event_Sequence_Type >, 2 > events_ptr;
    //std::array< Event_Sequence_Type, 2 > events;

    const std::vector< fast5::EventDetection_Event_Entry >& ed_events() const
    {
        assert(ed_events_ptr);
        return *ed_events_ptr;
    }
    std::vector< fast5::EventDetection_Event_Entry >& ed_events()
    {
        assert(ed_events_ptr);
        return *ed_events_ptr;
    }
    const Event_Sequence_Type& events(unsigned st) const
    {
        assert(st < 2);
        assert(events_ptr[st]);
        return *events_ptr[st];
    }
    Event_Sequence_Type& events(unsigned st)
    {
        assert(st < 2);
        assert(events_ptr[st]);
        return *events_ptr[st];
    }

    static unsigned& min_ed_events()
    {
        static unsigned _min_ed_events = 10;
        return _min_ed_events;
    }

    static unsigned& max_ed_events()
    {
        static unsigned _max_ed_events = 100000;
        return _max_ed_events;
    }

    static std::string& eventdetection_group()
    {
        static std::string _eventdetection_group = "000";
        return _eventdetection_group;
    }

    // percent of top events to ignore
    static double& abasic_level_top_percent()
    {
        static double _abasic_level_top_percent = 1.0;
        return _abasic_level_top_percent;
    }

    // what to add to top level
    static double& abasic_level_top_offset()
    {
        static double _abasic_level_top_offset = 0.0;
        return _abasic_level_top_offset;
    }

    // window size to consider for hairpin detection
    static unsigned& hairpin_island_window_size()
    {
        static unsigned _hairpin_island_window_size = 10;
        return _hairpin_island_window_size;
    }

    // window load to consider for hairpin detection
    static unsigned& hairpin_island_window_load()
    {
        static unsigned _hairpin_island_window_load = 5;
        return _hairpin_island_window_load;
    }

    // if set, do not split strands
    static unsigned& template_only()
    {
        static unsigned _template_only = 0;
        return _template_only;
    }

    // trim margins: after start, before end, before hairpin start, after hairpin end
    static std::array< unsigned, 4 >& trim_margins()
    {
        static std::array< unsigned, 4 > _trim_margins = {{ 50u, 50u, 50u, 50u }};
        return _trim_margins;
    }

    Fast5_Summary() : valid(false) {}
    Fast5_Summary(const std::string fn, const Pore_Model_Dict_Type& models, bool sst)
        : valid(false) { summarize(fn, models, sst); }

    void summarize(const std::string& fn, const Pore_Model_Dict_Type& models, bool sst)
    {
        valid = true;
        // initialize fields
        file_name = fn;
        auto pos = file_name.find_last_of('/');
        base_file_name = (pos != std::string::npos? file_name.substr(pos + 1) : file_name);
        if (base_file_name.substr(base_file_name.size() - 6) == ".fast5")
        {
            base_file_name.resize(base_file_name.size() - 6);
        }
        read_id = base_file_name;
        strand_bounds = {{ 0, 0, 0, 0 }};
        time_length = {{ 0.0, 0.0 }};
        num_ed_events = 0;
        abasic_level = 0.0;
        fast5::File f;
        do
        {
            try
            {
                // open file
                f.open(file_name); // can throw
                // get sampling rate
                if (not f.have_sampling_rate())
                {
                    LOG("Fast5_Summary", info) << file_name << ": missing sampling rate" << std::endl;
                    break;
                }
                sampling_rate = f.get_sampling_rate(); // can throw
                if (sampling_rate < 1000.0 or sampling_rate > 10000.0)
                {
                    LOG("Fast5_Summary", warning) << file_name << ": unexpected sampling rate: " << sampling_rate << std::endl;
                    break;
                }
                // get ed event params and ed events
                if (not f.have_eventdetection_events(eventdetection_group()))
                {
                    LOG("Fast5_Summary", info) << file_name << ": missing eventdetection events" << std::endl;
                    break;
                }
                auto ed_params = f.get_eventdetection_event_params(eventdetection_group()); // can throw
                if (not ed_params.read_id.empty())
                {
                    read_id = ed_params.read_id;
                }
                load_ed_events(&f); // also sets num_ed_events
                if (num_ed_events < trim_margins()[0] + trim_margins()[1] + min_ed_events())
                {
                    LOG("Fast5_Summary", info)
                        << file_name << ": not enough eventdetection events: " << num_ed_events << std::endl;
                    num_ed_events = 0;
                    break;
                }
                // get abasic level
                abasic_level = detect_abasic_level();
                if (abasic_level <= 1.0)
                {
                    LOG("Fast5_Summary", info)
                        << file_name << ": abasic level too low: " << abasic_level << std::endl;
                    num_ed_events = 0;
                    break;
                }
                // detect strands
                strand_bounds = {{ trim_margins()[0], num_ed_events - trim_margins()[1], 0, 0 }};
                if (not template_only()) detect_strands();
                if (strand_bounds[1] <= strand_bounds[0])
                {
                    LOG("Fast5_Summary", info) << file_name << ": no template strand detected" << std::endl;
                    num_ed_events = 0;
                    break;
                }
                scale_strands_together = (sst
                                          and strand_bounds[1] - strand_bounds[0] >= min_ed_events()
                                          and strand_bounds[3] - strand_bounds[2] >= min_ed_events());
                // compute time lengths
                load_events(&f);
                for (unsigned st = 0; st < 2; ++st)
                {
                    if (events(st).size() < min_ed_events()) continue;
                    time_length[st] = events(st).rbegin()->start + events(st).rbegin()->length;
                }
                //
                // compute initial model scalings
                //
                if (scale_strands_together)
                {
                    auto r0 = alg::mean_stdv_of< Float_Type >(
                        events(0),
                        [] (const Event_Type& ev) { return ev.mean; });
                    auto r1 = alg::mean_stdv_of< Float_Type >(
                        events(1),
                        [] (const Event_Type& ev) { return ev.mean; });
                    for (const auto& p0 : models)
                        if (p0.second.strand() == 0 or p0.second.strand() == 2)
                            for (const auto& p1 : models)
                                if (p1.second.strand() == 1 or p1.second.strand() == 2)
                                {
                                    std::array< std::string, 2 > m_name = {{ p0.first, p1.first }};
                                    Pore_Model_Parameters_Type pm_params;
                                    pm_params.scale = (r0.second / p0.second.stdv()
                                                       + r1.second / p1.second.stdv()) / 2;
                                    pm_params.shift = (r0.first - pm_params.scale * p0.second.mean()
                                                       + r1.first - pm_params.scale * p1.second.mean()) / 2;
                                    LOG("Fast5_Summary", debug)
                                        << "initial_scaling read [" << read_id
                                        << "] strand [2] model [" << m_name[0] << "+" << m_name[1]
                                        << "] pm_params [" << pm_params << "]" << std::endl;
                                    pm_params_m[m_name] = std::move(pm_params);
                                    st_params_m[m_name][0] = State_Transition_Parameters_Type();
                                    st_params_m[m_name][1] = State_Transition_Parameters_Type();
                                }
                }
                else // not scale_strands_together
                {
                    for (unsigned st = 0; st < 2; ++st)
                    {
                        if (events(st).size() < min_ed_events()) continue;
                        auto r = alg::mean_stdv_of< Float_Type >(
                            events(st),
                            [] (const Event_Type& ev) { return ev.mean; });
                        for (const auto& p : models)
                        {
                            if (p.second.strand() == st or p.second.strand() == 2)
                            {
                                std::array< std::string, 2 > m_name;
                                m_name[st] = p.first;
                                Pore_Model_Parameters_Type pm_params;
                                pm_params.scale = r.second / p.second.stdv();
                                pm_params.shift = r.first - pm_params.scale * p.second.mean();
                                LOG("Fast5_Summary", debug)
                                    << "initial_scaling read [" << read_id
                                    << "] strand [" << st
                                    << "] model [" << m_name[st]
                                    << "] pm_params [" << pm_params << "]" << std::endl;
                                pm_params_m[m_name] = std::move(pm_params);
                                st_params_m[m_name][st] = State_Transition_Parameters_Type();
                            }
                        }
                    }
                }
                // detect basecall group to write
                auto bc_grp_l = f.get_basecall_group_list();
                static const std::string bc_grp_prefix("Nanocall_");
                std::set< std::string > used_tags;
                for (const auto& bc_grp : bc_grp_l)
                {
                    if (bc_grp.size() <= bc_grp_prefix.size()) continue;
                    auto p = std::mismatch(bc_grp_prefix.begin(),
                                           bc_grp_prefix.end(),
                                           bc_grp.begin());
                    if (p.first != bc_grp_prefix.end()) continue;
                    std::string tag(p.second, bc_grp.end());
                    std::clog << "found basecall group: " << tag << std::endl;
                    used_tags.emplace(std::move(tag));
                }
                for (unsigned i = 0; i < 1000; ++i)
                {
                    std::ostringstream tmp;
                    tmp << std::setw(3) << std::setfill('0') << i;
                    if (not used_tags.count(tmp.str()))
                    {
                        bc_grp = bc_grp_prefix + tmp.str();
                        break;
                    }
                }
                if (bc_grp.empty())
                {
                    LOG(error)
                        << "no available basecall tag" << std::endl;
                    std::exit(EXIT_FAILURE);
                }
            }
            catch (hdf5_tools::Exception& e)
            {
                LOG(warning) << file_name << ": HDF5 error: " << e.what() << std::endl;
                num_ed_events = 0;
            }
        } while (false);
        drop_events();
        ed_events_ptr.reset();
    } // summarize

    void load_events(fast5::File* f_p = nullptr)
    {
        assert(valid);
        drop_events();
        if (num_ed_events == 0)
        {
            return;
        }
        bool must_load_ed_events = not ed_events_ptr;
        if (must_load_ed_events)
        {
#ifndef H5_HAVE_THREADSAFE
            static std::mutex fast5_mutex;
            std::lock_guard< std::mutex > fast5_lock(fast5_mutex);
#endif
            bool must_open_file = not f_p;
            if (must_open_file)
            {
                f_p = new fast5::File(file_name);
            }
            assert(f_p->is_open());
            load_ed_events(f_p);
            if (must_open_file)
            {
                delete f_p;
            }
        }
        for (unsigned st = 0; st < 2; ++st)
        {
            events_ptr[st] = typename decltype(events_ptr)::value_type(new typename decltype(events_ptr)::value_type::element_type ());
            for (unsigned j = strand_bounds[2 * st]; j < strand_bounds[2 * st + 1]; ++j)
            {
                if (filter_ed_event(ed_events()[j], abasic_level))
                {
                    Event_Type e;
                    e.mean = ed_events()[j].mean;
                    e.corrected_mean = e.mean;
                    e.stdv = ed_events()[j].stdv;
                    e.start = (ed_events()[j].start - ed_events()[strand_bounds[scale_strands_together? 0 : 2 * st]].start) / sampling_rate;
                    e.length = ed_events()[j].length / sampling_rate;
                    e.update_logs();
                    events(st).emplace_back(std::move(e));
                }
            }
        }
        if (must_load_ed_events)
        {
            ed_events_ptr.reset();
        }
    }
    void drop_events()
    {
        for (unsigned st = 0; st < 2; ++st)
        {
            events_ptr[st].reset();
        }
    }

    void add_basecall_seq(const std::string& name, unsigned st, const std::string& seq, int default_qual = 33) const
    {
        try
        {
            // open file
            fast5::File f(file_name, true); // can throw
            // write seq
            f.add_basecall_seq(st, bc_grp, name, seq, default_qual);
        }
        catch (hdf5_tools::Exception& e)
        {
            LOG(warning) << file_name << ": HDF5 error: " << e.what() << std::endl;
        }
    }

    void add_basecall_events(unsigned st, const Event_Sequence_Type& ev) const
    {
        try
        {
            // open file
            fast5::File f(file_name, true); // can throw
            // write seq
            f.add_basecall_events(st, bc_grp, ev);
        }
        catch (hdf5_tools::Exception& e)
        {
            LOG(warning) << file_name << ": HDF5 error: " << e.what() << std::endl;
        }
    }

    void add_basecall_model(unsigned st, const Pore_Model_Type& model) const
    {
        try
        {
            // open file
            fast5::File f(file_name, true); // can throw
            // write model params
            f.add_basecall_model(st, bc_grp, model.get_state_vector());
        }
        catch (hdf5_tools::Exception& e)
        {
            LOG(warning) << file_name << ": HDF5 error: " << e.what() << std::endl;
        }
    }

    void add_basecall_model_params(unsigned st, const Pore_Model_Parameters_Type& params) const
    {
        try
        {
            // open file
            fast5::File f(file_name, true); // can throw
            // write model params
            f.add_basecall_model_params(st, bc_grp, params);
        }
        catch (hdf5_tools::Exception& e)
        {
            LOG(warning) << file_name << ": HDF5 error: " << e.what() << std::endl;
        }
    }

    friend std::ostream& operator << (std::ostream& os, const Fast5_Summary& fs)
    {
        os << "[base_file_name=" << fs.base_file_name << " valid=" << fs.valid;
        if (fs.valid)
        {
            os << " num_ed_events=" << fs.num_ed_events;
            if (fs.num_ed_events > 0)
            {
                os << " read_id=" << fs.read_id
                   << " abasic_level=" << fs.abasic_level
                   << " strand_bounds=[" << fs.strand_bounds[0] << ","
                   << fs.strand_bounds[1] << ","
                   << fs.strand_bounds[2] << ","
                   << fs.strand_bounds[3]
                   << "] time_length=[" << fs.time_length[0] << "," << fs.time_length[1] << "]";
            }
        }
        os << "]";
        return os;
    }

    static void write_tsv_header(std::ostream& os)
    {
        os << "file_name" << "\tread_name" << "\tnum_ed_events" << "\tabasic_level"
           << "\ttemplate_start_idx" << "\ttemplate_end_idx"
           << "\tcomplement_start_idx" << "\tcomplement_end_idx";
        for (unsigned st = 0; st < 2; ++st)
        {
            os << "\tn" << st << "_model_name"
               << "\tn" << st << "_scale"
               << "\tn" << st << "_shift"
               << "\tn" << st << "_drift"
               << "\tn" << st << "_var"
               << "\tn" << st << "_scale_sd"
               << "\tn" << st << "_var_sd"
               << "\tn" << st << "_p_stay"
               << "\tn" << st << "_p_skip";
        }
    }

    void write_tsv(std::ostream& os) const
    {
        os << base_file_name << '\t' << read_id << '\t' << num_ed_events << '\t' << abasic_level
           << '\t' << strand_bounds[0] << '\t' << strand_bounds[1]
           << '\t' << strand_bounds[2] << '\t' << strand_bounds[3];
        for (unsigned st = 0; st < 2; ++st)
        {
            os << '\t';
            if (not preferred_model[st][st].empty())
            {
                os << preferred_model[st][st] << '\t';
                pm_params_m.at(preferred_model[st]).write_tsv(os);
                os << '\t';
                st_params_m.at(preferred_model[st])[st].write_tsv(os);
            }
            else
            {
                os << ".\t";
                Pore_Model_Parameters_Type().write_tsv(os);
                os << '\t';
                State_Transition_Parameters_Type().write_tsv(os);
            }
        }
    }

private:
    void load_ed_events(fast5::File* f_p)
    {
        ed_events_ptr = decltype(ed_events_ptr)(
            new typename decltype(ed_events_ptr)::element_type(
                f_p->get_eventdetection_events(eventdetection_group())));
        if (num_ed_events == 0)
        {
            if (ed_events().size() > max_ed_events())
            {
                LOG("Fast5_Summary", info)
                    << file_name << ": using only " << max_ed_events()
                    << " of " << ed_events().size() << " events" << std::endl;
                num_ed_events = max_ed_events();
            }
            else
            {
                num_ed_events = ed_events().size();
            }
        }
        ed_events().resize(num_ed_events);
    }

    // crude detection of abasic level
    Float_Type detect_abasic_level()
    {
        //
        // exclude top abasic_level_top_percent() levels
        // add abasic_level_top_offset()
        //
        std::vector< Float_Type > s;
        s.resize(ed_events().size());
        unsigned i;
        for (i = 0; i < ed_events().size(); ++i)
        {
            s[i] = ed_events()[i].mean;
        }
        std::sort(s.begin(), s.end());
        return s[(double)s.size() * (1.0 - abasic_level_top_percent() / 100.0)] + abasic_level_top_offset();
    } // detect_abasic_level()

    std::vector< std::pair< unsigned, unsigned > > find_islands_5_consec() const
    {
        //
        // find islands of >= 5 consecutive events at high level
        //
        std::vector< std::pair< unsigned, unsigned > > islands;
        unsigned i = 0;
        while (i < ed_events().size())
        {
            if (ed_events()[i].mean >= abasic_level)
            {
                unsigned j = i + 1;
                while (j < ed_events().size() and ed_events()[j].mean >= abasic_level) ++j;
                if (j - i >= 5)
               {
                    islands.push_back(std::make_pair(i, j));
                    LOG("Fast5_Summary", debug) << "abasic_island [" << i << "," << j << "]" << std::endl;
                }
                i = j + 1;
            }
            else
            {
                ++i;
            }
        }
        return islands;
    }

    std::vector< std::pair< unsigned, unsigned > > find_islands_5_of_10_consec() const
    {
        //
        // find islands of >= 5/10 consecutive events at high level
        //
        std::vector< std::pair< unsigned, unsigned > > islands;
        unsigned i = 0;
        unsigned window_start = 0;
        unsigned window_count = 0;
        while (i < ed_events().size())
        {
            if (ed_events()[i].mean >= abasic_level)
            {
                while (window_start + 10 <= i)
                {
                    if (ed_events()[window_start].mean >= abasic_level)
                    {
                        --window_count;
                    }
                    ++window_start;
                }
                while (window_start < i and ed_events()[window_start].mean < abasic_level)
                {
                    ++window_start;
                }
                assert(i < window_start + 10);
                ++window_count;
                if (window_count >= 5)
                {
                    islands.push_back(std::make_pair(window_start, i));
                    LOG("Fast5_Summary", debug) << "abasic_island [" << window_start << "," << i << "]" << std::endl;
                    window_start = i + 1;
                    window_count = 0;
                }
            }
            ++i;
        }
        return islands;
    }

    // crude detection of hairpin islands
    // look for >= hairping_window_load/hairpin_window_size consecutive events at high level
    std::vector< std::pair< unsigned, unsigned > > find_hairpin_islands() const
    {
        std::vector< std::pair< unsigned, unsigned > > islands;
        unsigned i = 0;
        unsigned window_start = 0;
        unsigned window_count = 0;
        while (i < ed_events().size())
        {
            if (ed_events()[i].mean >= abasic_level)
            {
                while (window_start + hairpin_island_window_size() <= i)
                {
                    if (ed_events()[window_start].mean >= abasic_level)
                    {
                        --window_count;
                    }
                    ++window_start;
                }
                while (window_start < i and ed_events()[window_start].mean < abasic_level)
                {
                    ++window_start;
                }
                assert(i < window_start + hairpin_island_window_size());
                ++window_count;
                if (window_count >= hairpin_island_window_load())
                {
                    islands.push_back(std::make_pair(window_start, i));
                    LOG("Fast5_Summary", debug) << "abasic_island [" << window_start << "," << i << "]" << std::endl;
                    window_start = i + 1;
                    window_count = 0;
                }
            }
            ++i;
        }
        return islands;
    } // find_hairpin_islands()

    // crude detection of strands in event sequence
    void detect_strands()
    {
        LOG("Fast5_Summary", debug)
            << "num_events=" << ed_events().size()
            << " abasic_level=" << abasic_level << std::endl;
        //
        // find islands of consecutive events at high level
        //
        auto islands = find_islands_5_consec(); //find_hairpin_islands();
        //
        // merge islands within 50bp of each other
        //
        for (unsigned i = 1; i < islands.size(); ++i)
        {
            if (islands[i - 1].second + std::max(trim_margins()[2], trim_margins()[3]) >= islands[i].first)
            {
                LOG("Fast5_Summary", debug) << "merge_islands "
                          << "[" << islands[i - 1].first << "," << islands[i - 1].second << "] with "
                          << "[" << islands[i].first << "," << islands[i].second << "]" << std::endl;
                islands[i - 1].second = islands[i].second;
                islands.erase(islands.begin() + i);
                i = 0;
            }
        }
        LOG("Fast5_Summary", debug)
            << "final_islands: " << alg::os_join(
                islands, " ",
                [] (const std::pair< unsigned, unsigned >& p) {
                    std::ostringstream tmp;
                    tmp << "[" << p.first << "," << p.second << "]";
                    return tmp.str();
                }) << std::endl;
        if (islands.empty())
        {
            LOG("Fast5_Summary", info)
                << "template_only read_id=[" << read_id << "]" << std::endl;
            return;
        }
        //
        // pick island closest to the middle of the event sequence
        //
        auto dist_to_middle = [&] (const std::pair< unsigned, unsigned >& p) {
            return std::min((unsigned)std::abs((long)p.first - (long)ed_events().size() / 2),
                            (unsigned)std::abs((long)p.second - (long)ed_events().size() / 2));
        };
        auto it = alg::min_of(islands, dist_to_middle);
        // check island is in the middle third; if not, intepret it as template only
        if (dist_to_middle(*it) > ed_events().size() / 6)
        {
            LOG("Fast5_Summary", info)
                << "drop_read read_id=[" << read_id
                << "] islands=[" << alg::os_join(
                    islands, " ",
                    [] (const std::pair< unsigned, unsigned >& p) {
                        std::ostringstream tmp;
                        tmp << "[" << p.first << "," << p.second << "]";
                        return tmp.str();
                    })
                << "]" << std::endl;
            return;
        }
        else
        {
            LOG("Fast5_Summary", debug)
                << "hairpin_island [" << it->first << "," << it->second << "]" << std::endl;
            strand_bounds[0] = trim_margins()[0];
            if (islands[0].first < trim_margins()[0] + trim_margins()[2])
            {
                strand_bounds[0] = std::max(strand_bounds[0], islands[0].second);
            }
            strand_bounds[1] = it->first - trim_margins()[2];
            strand_bounds[2] = it->first + trim_margins()[3];
            strand_bounds[3] = ed_events().size() - trim_margins()[1];
            if (islands[islands.size() - 1].second > ed_events().size() - (trim_margins()[3] + trim_margins()[1]))
            {
                strand_bounds[3] = std::min(strand_bounds[3], islands[islands.size() - 1].first);
            }
        }
    } // detect_strands()

    // crude filtering of eventdetection events
    static bool filter_ed_event(const fast5::EventDetection_Event_Entry& e, Float_Type abasic_level)
    {
        if (e.mean >= abasic_level)
        {
            return false;
        }
        if (e.stdv > 4.0)
        {
            return false;
        }
        return true;
    } // filter_ed_event()
}; // struct Fast5_Summary

#endif
