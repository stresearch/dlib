// Copyright (C) 2011  Davis E. King (davis@dlib.net)
// License: Boost Software License   See LICENSE.txt for the full license.
#ifndef DLIB_STRUCTURAL_SVM_ObJECT_DETECTION_PROBLEM_H__
#define DLIB_STRUCTURAL_SVM_ObJECT_DETECTION_PROBLEM_H__

#include "structural_svm_object_detection_problem_abstract.h"
#include "../matrix.h"
#include "structural_svm_problem_threaded.h"
#include <sstream>
#include "../string.h"
#include "../array.h"
#include "../image_processing/full_object_detection.h"

namespace dlib
{

// ----------------------------------------------------------------------------------------

    class impossible_labeling_error : public dlib::error 
    { 
    public: 
        impossible_labeling_error(const std::string& msg) : dlib::error(msg) {};
    };

// ----------------------------------------------------------------------------------------

    template <
        typename image_scanner_type,
        typename overlap_tester_type,
        typename image_array_type 
        >
    class structural_svm_object_detection_problem : public structural_svm_problem_threaded<matrix<double,0,1> >,
                                                    noncopyable
    {
    public:

        structural_svm_object_detection_problem(
            const image_scanner_type& scanner,
            const overlap_tester_type& overlap_tester,
            const image_array_type& images_,
            const std::vector<std::vector<full_object_detection> >& truth_object_detections_,
            unsigned long num_threads = 2
        ) :
            structural_svm_problem_threaded<matrix<double,0,1> >(num_threads),
            boxes_overlap(overlap_tester),
            images(images_),
            truth_object_detections(truth_object_detections_),
            match_eps(0.5),
            loss_per_false_alarm(1),
            loss_per_missed_target(1)
        {
#ifdef ENABLE_ASSERTS
            // make sure requires clause is not broken
            DLIB_ASSERT(is_learning_problem(images_, truth_object_detections_) && 
                         scanner.get_num_detection_templates() > 0,
                "\t structural_svm_object_detection_problem::structural_svm_object_detection_problem()"
                << "\n\t Invalid inputs were given to this function "
                << "\n\t scanner.get_num_detection_templates(): " << scanner.get_num_detection_templates()
                << "\n\t is_learning_problem(images_,truth_object_detections_): " << is_learning_problem(images_,truth_object_detections_)
                << "\n\t this: " << this
                );
            for (unsigned long i = 0; i < truth_object_detections.size(); ++i)
            {
                for (unsigned long j = 0; j < truth_object_detections[i].size(); ++j)
                {
                    DLIB_ASSERT(truth_object_detections[i][j].movable_parts.size() == scanner.get_num_movable_components_per_detection_template(),
                        "\t trained_function_type structural_object_detection_trainer::train()"
                        << "\n\t invalid inputs were given to this function"
                        << "\n\t truth_object_detections["<<i<<"]["<<j<<"].movable_parts.size():          " << 
                            truth_object_detections[i][j].movable_parts.size()
                        << "\n\t scanner.get_num_movable_components_per_detection_template(): " << 
                            scanner.get_num_movable_components_per_detection_template()
                    );
                }
            }
#endif

            scanners.set_max_size(images.size());
            scanners.set_size(images.size());

            max_num_dets = 0;
            for (unsigned long i = 0; i < truth_object_detections.size(); ++i)
            {
                if (truth_object_detections[i].size() > max_num_dets)
                    max_num_dets = truth_object_detections[i].size();

                scanners[i].copy_configuration(scanner);
            }
            max_num_dets = max_num_dets*3 + 10;
        }

        void set_match_eps (
            double eps
        )
        {
            // make sure requires clause is not broken
            DLIB_ASSERT(0 < eps && eps < 1, 
                "\t void structural_svm_object_detection_problem::set_match_eps(eps)"
                << "\n\t Invalid inputs were given to this function "
                << "\n\t eps:  " << eps 
                << "\n\t this: " << this
                );

            match_eps = eps;
        }

        double get_match_eps (
        ) const
        {
            return match_eps;
        }

        double get_loss_per_missed_target (
        ) const
        {
            return loss_per_missed_target;
        }

        void set_loss_per_missed_target (
            double loss
        )
        {
            // make sure requires clause is not broken
            DLIB_ASSERT(loss > 0, 
                "\t void structural_svm_object_detection_problem::set_loss_per_missed_target(loss)"
                << "\n\t Invalid inputs were given to this function "
                << "\n\t loss: " << loss
                << "\n\t this: " << this
                );

            loss_per_missed_target = loss;
        }

        double get_loss_per_false_alarm (
        ) const
        {
            return loss_per_false_alarm;
        }

        void set_loss_per_false_alarm (
            double loss
        )
        {
            // make sure requires clause is not broken
            DLIB_ASSERT(loss > 0, 
                "\t void structural_svm_object_detection_problem::set_loss_per_false_alarm(loss)"
                << "\n\t Invalid inputs were given to this function "
                << "\n\t loss: " << loss
                << "\n\t this: " << this
                );

            loss_per_false_alarm = loss;
        }

    private:
        virtual long get_num_dimensions (
        ) const 
        {
            return scanners[0].get_num_dimensions() + 
                1;// for threshold
        }

        virtual long get_num_samples (
        ) const 
        {
            return images.size();
        }

        virtual void get_truth_joint_feature_vector (
            long idx,
            feature_vector_type& psi 
        ) const 
        {
            const image_scanner_type& scanner = get_scanner(idx);

            psi.set_size(get_num_dimensions());
            std::vector<rectangle> mapped_rects;

            psi = 0;
            for (unsigned long i = 0; i < truth_object_detections[idx].size(); ++i)
            {
                mapped_rects.push_back(scanner.get_best_matching_rect(truth_object_detections[idx][i].rect));
                scanner.get_feature_vector(truth_object_detections[idx][i], psi);
            }
            psi(scanner.get_num_dimensions()) = -1.0*truth_object_detections[idx].size();

            // check if any of the boxes overlap.  If they do then it is impossible for
            // us to learn to correctly classify this sample
            for (unsigned long i = 0; i < mapped_rects.size(); ++i)
            {
                for (unsigned long j = i+1; j < mapped_rects.size(); ++j)
                {
                    if (boxes_overlap(mapped_rects[i], mapped_rects[j]))
                    {
                        const double area_overlap = mapped_rects[i].intersect(mapped_rects[j]).area();
                        const double match_amount = area_overlap/(double)( mapped_rects[i]+mapped_rects[j]).area();
                        const double overlap_amount = area_overlap/std::min(mapped_rects[i].area(),mapped_rects[j].area());

                        using namespace std;
                        ostringstream sout;
                        sout << "An impossible set of object labels was detected. This is happening because ";
                        sout << "the truth labels for an image contain rectangles which overlap according to the ";
                        sout << "overlap_tester_type supplied for non-max suppression.  To resolve this, you either need to ";
                        sout << "relax the overlap tester so it doesn't mark these rectangles as overlapping ";
                        sout << "or adjust the truth rectangles. ";

                        // make sure the above string fits nicely into a command prompt window.
                        string temp = sout.str();
                        sout.str(""); sout << wrap_string(temp,0,0) << endl << endl;


                        sout << "image index: "<< idx << endl;
                        sout << "The offending rectangles are:\n";
                        sout << "rect1: "<< mapped_rects[i] << endl;
                        sout << "rect2: "<< mapped_rects[j] << endl;
                        sout << "match amount:   " << match_amount << endl;
                        sout << "overlap amount: " << overlap_amount << endl;
                        throw dlib::impossible_labeling_error(sout.str());
                    }
                }
            }

            // make sure the mapped rectangles are within match_eps of the
            // truth rectangles.
            for (unsigned long i = 0; i < mapped_rects.size(); ++i)
            {
                const double area = (truth_object_detections[idx][i].rect.intersect(mapped_rects[i])).area();
                const double total_area = (truth_object_detections[idx][i].rect + mapped_rects[i]).area();
                if (area/total_area <= match_eps)
                {
                    using namespace std;
                    ostringstream sout;
                    sout << "An impossible set of object labels was detected.  This is happening because ";
                    sout << "none of the sliding window detection templates is capable of matching the size ";
                    sout << "and/or shape of one of the ground truth rectangles to within the required match_eps ";
                    sout << "amount of alignment.  To resolve this you need to either lower the match_eps, add ";
                    sout << "another detection template which can match the offending rectangle, or adjust the ";
                    sout << "offending truth rectangle so it can be matched by an existing detection template. ";
                    sout << "It is also possible that the image pyramid you are using is too coarse.  E.g. if one of ";
                    sout << "your existing detection templates has a matching width/height ratio and smaller area ";
                    sout << "than the offending rectangle then a finer image pyramid would probably help.";


                    // make sure the above string fits nicely into a command prompt window.
                    string temp = sout.str();
                    sout.str(""); sout << wrap_string(temp,0,0) << endl << endl;

                    sout << "image index              "<< idx << endl;
                    sout << "match_eps:               "<< match_eps << endl;
                    sout << "best possible match:     "<< area/total_area << endl;
                    sout << "truth rect:              "<< truth_object_detections[idx][i].rect << endl;
                    sout << "truth rect width/height: "<< truth_object_detections[idx][i].rect.width()/(double)truth_object_detections[idx][i].rect.height() << endl;
                    sout << "truth rect area:         "<< truth_object_detections[idx][i].rect.area() << endl;
                    sout << "nearest detection template rect:              "<< mapped_rects[i] << endl;
                    sout << "nearest detection template rect width/height: "<< mapped_rects[i].width()/(double)mapped_rects[i].height() << endl;
                    sout << "nearest detection template rect area:         "<< mapped_rects[i].area() << endl;
                    throw dlib::impossible_labeling_error(sout.str());
                }

            }
        }

        virtual void separation_oracle (
            const long idx,
            const matrix_type& current_solution,
            scalar_type& loss,
            feature_vector_type& psi
        ) const 
        {
            const image_scanner_type& scanner = get_scanner(idx);

            std::vector<std::pair<double, rectangle> > dets;
            const double thresh = current_solution(scanner.get_num_dimensions());


            scanner.detect(current_solution, dets, thresh-loss_per_false_alarm);


            // The loss will measure the number of incorrect detections.  A detection is
            // incorrect if it doesn't hit a truth rectangle or if it is a duplicate detection
            // on a truth rectangle.
            loss = truth_object_detections[idx].size()*loss_per_missed_target;

            // Measure the loss augmented score for the detections which hit a truth rect.
            std::vector<double> truth_score_hits(truth_object_detections[idx].size(), 0);

            // keep track of which truth boxes we have hit so far.
            std::vector<bool> hit_truth_table(truth_object_detections[idx].size(), false);

            std::vector<rectangle> final_dets;
            // The point of this loop is to fill out the truth_score_hits array. 
            for (unsigned long i = 0; i < dets.size() && final_dets.size() < max_num_dets; ++i)
            {
                if (overlaps_any_box(final_dets, dets[i].second))
                    continue;

                const std::pair<double,unsigned int> truth = find_best_match(truth_object_detections[idx], dets[i].second);

                final_dets.push_back(dets[i].second);

                const double truth_match = truth.first;
                // if hit truth rect
                if (truth_match > match_eps)
                {
                    // if this is the first time we have seen a detect which hit truth_object_detections[truth.second]
                    const double score = dets[i].first - thresh;
                    if (hit_truth_table[truth.second] == false)
                    {
                        hit_truth_table[truth.second] = true;
                        truth_score_hits[truth.second] += score;
                    }
                    else
                    {
                        truth_score_hits[truth.second] += score + loss_per_false_alarm;
                    }
                }
            }

            hit_truth_table.assign(hit_truth_table.size(), false);

            final_dets.clear();
#ifdef ENABLE_ASSERTS
            double total_score = 0;
#endif
            // Now figure out which detections jointly maximize the loss and detection score sum.  We
            // need to take into account the fact that allowing a true detection in the output, while 
            // initially reducing the loss, may allow us to increase the loss later with many duplicate
            // detections.
            for (unsigned long i = 0; i < dets.size() && final_dets.size() < max_num_dets; ++i)
            {
                if (overlaps_any_box(final_dets, dets[i].second))
                    continue;

                const std::pair<double,unsigned int> truth = find_best_match(truth_object_detections[idx], dets[i].second);

                const double truth_match = truth.first;
                if (truth_match > match_eps)
                {
                    if (truth_score_hits[truth.second] > loss_per_missed_target)
                    {
                        if (!hit_truth_table[truth.second])
                        {
                            hit_truth_table[truth.second] = true;
                            final_dets.push_back(dets[i].second);
#ifdef ENABLE_ASSERTS
                            total_score += dets[i].first;
#endif
                            loss -= loss_per_missed_target;
                        }
                        else
                        {
                            final_dets.push_back(dets[i].second);
#ifdef ENABLE_ASSERTS
                            total_score += dets[i].first;
#endif
                            loss += loss_per_false_alarm;
                        }
                    }
                }
                else
                {
                    // didn't hit anything
                    final_dets.push_back(dets[i].second);
#ifdef ENABLE_ASSERTS
                    total_score += dets[i].first;
#endif
                    loss += loss_per_false_alarm;
                }
            }

            psi.set_size(get_num_dimensions());
            psi = 0;
            for (unsigned long i = 0; i < final_dets.size(); ++i)
                scanner.get_feature_vector(scanner.get_full_object_detection(final_dets[i], current_solution), psi);

#ifdef ENABLE_ASSERTS
            const double psi_score = dot(psi, current_solution);
            DLIB_ASSERT(std::abs(psi_score-total_score)*std::max(psi_score,total_score) < 1e-10,
                        "\t The get_feature_vector() and detect() methods of image_scanner_type are not in sync." 
                        << "\n\t The relative error is too large to be attributed to rounding error."
                        << "\n\t relative error: " << std::abs(psi_score-total_score)*std::max(psi_score,total_score)
                        << "\n\t psi_score:      " << psi_score
                        << "\n\t total_score:    " << total_score
            );
#endif

            psi(scanner.get_num_dimensions()) = -1.0*final_dets.size();
        }


        bool overlaps_any_box (
            const std::vector<rectangle>& truth_object_detections,
            const dlib::rectangle& rect
        ) const
        {
            for (unsigned long i = 0; i < truth_object_detections.size(); ++i)
            {
                if (boxes_overlap(truth_object_detections[i], rect))
                    return true;
            }
            return false;
        }

        std::pair<double,unsigned int> find_best_match(
            const std::vector<full_object_detection>& boxes,
            const rectangle rect
        ) const
        /*!
            ensures
                - determines which rectangle in boxes matches rect the most and
                  returns the amount of this match.  Specifically, the match is
                  a number O with the following properties:
                    - 0 <= O <= 1
                    - Let R be the maximum matching rectangle in boxes, then
                      O == (R.intersect(rect)).area() / (R + rect).area()
                    - O == 0 if there is no match with any rectangle.
        !*/
        {
            double match = 0;
            unsigned int best_idx = 0;
            for (unsigned long i = 0; i < boxes.size(); ++i)
            {

                const unsigned long area = rect.intersect(boxes[i].rect).area();
                if (area != 0)
                {
                    const double new_match = area / static_cast<double>((rect + boxes[i].rect).area());
                    if (new_match > match)
                    {
                        match = new_match;
                        best_idx = i;
                    }
                }
            }

            return std::make_pair(match,best_idx);
        }


        const image_scanner_type& get_scanner (long idx) const
        {
            if (scanners[idx].is_loaded_with_image() == false)
                scanners[idx].load(images[idx]);

            return scanners[idx];
        }


        overlap_tester_type boxes_overlap;

        mutable array<image_scanner_type> scanners;

        const image_array_type& images;
        const std::vector<std::vector<full_object_detection> >& truth_object_detections;

        unsigned long max_num_dets;
        double match_eps;
        double loss_per_false_alarm;
        double loss_per_missed_target;
    };

// ----------------------------------------------------------------------------------------

}

#endif // DLIB_STRUCTURAL_SVM_ObJECT_DETECTION_PROBLEM_H__


